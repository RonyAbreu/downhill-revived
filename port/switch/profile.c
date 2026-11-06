/* A sampling profiler for the Switch (finding 167, NX3): where the game and VU threads spend a
   field, measured on the console itself, which no PC tool reaches.

   `DHD_SWITCH_PROFILE = 1` in dhd.ini (or the environment) starts it. Every named thread
   (host_thread_name -> switch_thread_core -> switch_profile_thread) gives its real handle; a
   thread of the profiler's own, one priority step above the game's, wakes about every
   millisecond, pauses each of them (svcSetThreadActivity), reads its PC and LR
   (svcGetThreadContext3) and lets it go on. That is the kernel's own way for a process to look
   at its own threads -- libnx's threadPause/threadDumpContext are the same two calls -- and a
   paused thread is held only for the read.

   The counts go to `profile.txt` beside dhd.log at exit: one line per (thread, PC, LR) with each
   address as an offset into the module, so `port/switch/profile_report.py` can symbolize it
   against dhd.elf (addr2line, inlined frames kept). A thread waiting rather than running shows
   up in the kernel call it waits in (svcWaitSynchronization, svcWaitProcessWideKeyAtomic, ...),
   so the report says how much of a thread's time is work and how much waiting, without a
   second mechanism.

   Windows (finding 168): the last two seconds of samples are also kept in a ring, each with its
   time and, for a sample in a kernel wait, two more return addresses. When the renderer logs a
   slow field it asks for a window over that field (switch_profile_window), and what each thread
   did in it is counted apart and written to profile.txt as `window` and `ws` lines, numbered as
   the log line says, so a 200 ms field can be read on its own instead of drowned in the run. */
#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define PROFILE_THREADS 8   /* 3 bits of the slot key */
#define PROFILE_SLOTS (1u << 18)   /* (thread, PC, LR): far more than a run touches */

#define OUTSIDE 0xFFFFFFFull   /* 28 bits */

typedef struct { uint64_t key; uint32_t count; } Slot;   /* key: slot_key, 0 empty */

static struct {
    const char *name; Handle handle; uint64_t samples, failed;
    uint64_t waits, last_ticks, last_waits, last_samples;   /* for the run/wait line every 10 s */
    uint64_t stack_top;   /* the end of its stack's mapping: wait_add never reads past it */
} threads[PROFILE_THREADS];
/* The kernel calls a waiting thread sits in: libnx's stubs, a few instructions each. */
static const uintptr_t wait_stubs[] = {
    (uintptr_t)svcWaitSynchronization, (uintptr_t)svcWaitProcessWideKeyAtomic, (uintptr_t)svcArbitrateLock,
    (uintptr_t)svcSleepThread, (uintptr_t)svcWaitForAddress, (uintptr_t)svcSendSyncRequest,
    (uintptr_t)svcReplyAndReceive,
};

static int in_wait(uintptr_t pc)
{
    for (unsigned i = 0; i < sizeof wait_stubs / sizeof *wait_stubs; i++)
        if (pc - wait_stubs[i] < 32) return 1;
    return 0;
}
/* The ring of recent samples (the windows): about 1,000 a second for each of up to 8 threads. */
typedef struct { uint64_t tick; uint32_t pc, lr, up1, up2; uint32_t thread; } Recent;
#define RECENT 16384   /* about two seconds of seven threads */
static Recent *recent;
static volatile uint64_t recent_count;   /* samples written so far; the ring holds the last RECENT */
/* A window: its length and, per (thread, PC, LR, up1, up2), a count. */
#define WINDOWS 64
#define WINDOW_SLOTS 2048
typedef struct { Recent key; uint32_t count; } WindowSlot;
static struct { double ms; WindowSlot *slots; uint32_t used, samples; } windows[WINDOWS];
static unsigned window_count;
static void recent_add(const ThreadContext *ctx, int thread, int waiting);

static volatile int thread_count;
static Slot *slots;
static uint64_t dropped;
static uintptr_t module_base;
static Thread sampler;
static volatile int sampler_on, sampler_quit;
static Mutex register_lock;
static uint64_t ticks, started_tick, stopped_tick;

static int profile_wanted(void)
{
    const char *want = getenv("DHD_SWITCH_PROFILE");
    return want && *want && strcmp(want, "0") && strcasecmp(want, "off");
}

static void slot_add(uint64_t key)
{
    uint32_t i = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 47) & (PROFILE_SLOTS - 1);
    for (unsigned probe = 0; probe < 64; probe++, i = (i + 1) & (PROFILE_SLOTS - 1)) {
        if (slots[i].key == key) { slots[i].count++; return; }
        if (!slots[i].key) { slots[i].key = key; slots[i].count = 1; return; }
    }
    dropped++;
}

/* An address as an offset into the .nro, in 28 bits: everything we run is in it, libnx's svc
   stubs included (the module is under 64 MiB); anything else is the all-ones offset. */
static uint64_t module_offset(uint64_t address)
{
    uint64_t offset = address - module_base;
    return address >= module_base && offset < OUTSIDE ? offset : OUTSIDE;
}

/* A sample's key: where it was (PC) and where that function returns to (LR), so the report can
   name the callers of a hot leaf like memset or mutexLock. Bit 63: never the empty key. */
static uint64_t slot_key(uint64_t pc, uint64_t lr, int thread)
{
    return 1ull << 63 | module_offset(pc) << 31 | module_offset(lr) << 3 | (uint64_t)thread;
}

/* The time a thread has run on any core (InfoType_ThreadTickCount, 13.0.0+), in system ticks. */
static uint64_t thread_run_ticks(Handle h)
{
    u64 out = 0;
    if (R_FAILED(svcGetInfo(&out, InfoType_ThreadTickCount, h, UINT64_MAX)) &&
        R_FAILED(svcGetInfo(&out, InfoType_ThreadTickCountDeprecated, h, UINT64_MAX)))
        return 0;
    return out;
}

/* Every 10 s, per thread: the share of the wall clock it ran on a core (the kernel's count), the
   share of its samples in a kernel wait, and what is left -- neither running nor waiting is a
   thread ready to run with no core free for it, which samples alone cannot tell from work. */
static void report_shares(uint64_t wall_ticks)
{
    char line[512];
    int at = snprintf(line, sizeof line, "switch profile: 10 s, %% of the wall clock run/wait/ready:");
    for (int t = 0; t < thread_count && at < (int)sizeof line - 40; t++) {
        uint64_t run = thread_run_ticks(threads[t].handle), samples = threads[t].samples - threads[t].last_samples;
        double ran = wall_ticks ? 100.0 * (double)(run - threads[t].last_ticks) / (double)wall_ticks : 0;
        double waited = samples ? 100.0 * (double)(threads[t].waits - threads[t].last_waits) / (double)samples : 0;
        double ready = 100.0 - ran - waited;
        at += snprintf(line + at, sizeof line - at, " %s %.0f/%.0f/%.0f", threads[t].name, ran, waited,
                       ready > 0 ? ready : 0);
        threads[t].last_ticks = run;
        threads[t].last_waits = threads[t].waits;
        threads[t].last_samples = threads[t].samples;
    }
    fprintf(stderr, "%s\n", line);
}

/* A sample in a kernel wait or lock: whose? The LR names only libnx's mutexLock or
   condvarWaitTimeout, so the frame records (GCC keeps x29/x30 pairs in every non-leaf aarch64
   function) are walked a few calls up, read from the paused thread's stack while it is held.
   Each frame pointer must lie above the last and inside the thread's stack (between its stack
   pointer and the end of the mapping it registered from), or the walk stops. */
#define WAIT_DEPTH 5
typedef struct { uint32_t at[WAIT_DEPTH]; uint32_t thread, count; } WaitStack;
#define WAIT_SLOTS 4096
static WaitStack *wait_stacks;

static void wait_add(const ThreadContext *ctx, int thread)
{
    if (!wait_stacks) return;
    WaitStack s = {{0}, (uint32_t)thread, 0};
    s.at[0] = (uint32_t)module_offset(ctx->lr);
    uint64_t sp = ctx->sp, fp = ctx->fp;
    for (int d = 1; d < WAIT_DEPTH; d++) {
        if (fp < sp || fp + 16 > threads[thread].stack_top || fp & 15) break;
        const uint64_t *frame = (const uint64_t *)fp;
        s.at[d] = (uint32_t)module_offset(frame[1]);
        if (frame[0] <= fp) break;
        fp = frame[0];
    }
    uint64_t h = thread;
    for (int d = 0; d < WAIT_DEPTH; d++) h = (h ^ s.at[d]) * 0x9E3779B97F4A7C15ull;
    for (uint32_t i = (uint32_t)(h >> 52) & (WAIT_SLOTS - 1), probe = 0; probe < 32; probe++, i = (i + 1) & (WAIT_SLOTS - 1)) {
        WaitStack *w = &wait_stacks[i];
        if (w->count && w->thread == s.thread && !memcmp(w->at, s.at, sizeof s.at)) { w->count++; return; }
        if (!w->count) { s.count = 1; *w = s; return; }
    }
    dropped++;
}

/* The sampler, the thread still paused: the sample into the ring. A waiting sample keeps two
   frames above its LR (as wait_add walks them), since the LR alone names only libnx's wrapper. */
static void recent_add(const ThreadContext *ctx, int thread, int waiting)
{
    if (!recent) return;
    Recent r = { armGetSystemTick(), (uint32_t)module_offset(ctx->pc.x), (uint32_t)module_offset(ctx->lr), 0, 0,
                 (uint32_t)thread };
    if (waiting) {
        uint64_t sp = ctx->sp, fp = ctx->fp;
        uint32_t *up[2] = { &r.up1, &r.up2 };
        for (int d = 0; d < 2; d++) {
            if (fp < sp || fp + 16 > threads[thread].stack_top || fp & 15) break;
            const uint64_t *frame = (const uint64_t *)fp;
            *up[d] = (uint32_t)module_offset(frame[1]);
            if (frame[0] <= fp) break;
            fp = frame[0];
        }
    }
    recent[recent_count % RECENT] = r;
    __atomic_store_n(&recent_count, recent_count + 1, __ATOMIC_RELEASE);
}

unsigned switch_profile_window(double ms)
{
    if (!sampler_on || !recent || window_count >= WINDOWS || ms <= 0) return 0;
    WindowSlot *slots_here = calloc(WINDOW_SLOTS, sizeof *slots_here);
    if (!slots_here) return 0;
    uint64_t now = armGetSystemTick(), span = (uint64_t)(ms * 1e-3 * armGetSystemTickFreq());
    uint64_t count = __atomic_load_n(&recent_count, __ATOMIC_ACQUIRE);
    uint64_t oldest = count > RECENT - 256 ? count - (RECENT - 256) : 0;   /* leave room for the writer */
    uint32_t used = 0, samples = 0;
    for (uint64_t i = count; i-- > oldest;) {
        Recent r = recent[i % RECENT];
        if (now - r.tick > span) break;   /* older than the window (ticks only grow) */
        samples++;
        uint64_t h = r.thread;
        h = (h ^ r.pc) * 0x9E3779B97F4A7C15ull; h = (h ^ r.lr) * 0x9E3779B97F4A7C15ull;
        h = (h ^ r.up1) * 0x9E3779B97F4A7C15ull; h = (h ^ r.up2) * 0x9E3779B97F4A7C15ull;
        for (uint32_t j = (uint32_t)(h >> 53) & (WINDOW_SLOTS - 1), probe = 0; probe < 64; probe++, j = (j + 1) & (WINDOW_SLOTS - 1)) {
            WindowSlot *w = &slots_here[j];
            if (w->count && w->key.thread == r.thread && w->key.pc == r.pc && w->key.lr == r.lr && w->key.up1 == r.up1 &&
                w->key.up2 == r.up2) { w->count++; break; }
            if (!w->count) { w->key = r; w->count = 1; used++; break; }
        }
    }
    unsigned id = window_count;
    windows[id].ms = ms; windows[id].slots = slots_here; windows[id].used = used; windows[id].samples = samples;
    __atomic_store_n(&window_count, id + 1, __ATOMIC_RELEASE);   /* the exit's writer reads only whole ones */
    return id + 1;
}

static void sampler_main(void *unused)
{
    (void)unused;
    uint64_t period_start = armGetSystemTick(), period = armGetSystemTickFreq() * 10;
    while (!sampler_quit) {
        svcSleepThread(1000000);   /* 1 ms; Horizon's tick makes it about that */
        ticks++;
        uint64_t now = armGetSystemTick();
        if (now - period_start >= period) {
            report_shares(now - period_start);
            period_start = now;
        }
        int count = thread_count;
        for (int t = 0; t < count; t++) {
            Handle h = threads[t].handle;
            if (R_FAILED(svcSetThreadActivity(h, ThreadActivity_Paused))) { threads[t].failed++; continue; }
            ThreadContext ctx;
            Result rc = svcGetThreadContext3(&ctx, h);
            int waiting = R_SUCCEEDED(rc) && in_wait(ctx.pc.x);
            if (waiting) wait_add(&ctx, t);   /* while it is still held: its frames can't change */
            if (R_SUCCEEDED(rc)) recent_add(&ctx, t, waiting);
            svcSetThreadActivity(h, ThreadActivity_Runnable);
            if (R_FAILED(rc)) { threads[t].failed++; continue; }
            threads[t].samples++;
            if (waiting) threads[t].waits++;
            slot_add(slot_key(ctx.pc.x, ctx.lr, t));
        }
    }
}

/* A named thread (switch_thread_core): the ones worth sampling are the game's own. */
void switch_profile_thread(const char *name)
{
    static int checked, on;
    if (!checked) {
        checked = 1;
        on = profile_wanted();
        mutexInit(&register_lock);
    }
    if (!on || !name) return;
    mutexLock(&register_lock);
    if (!sampler_on && !slots) {
        slots = calloc(PROFILE_SLOTS, sizeof *slots);
        wait_stacks = calloc(WAIT_SLOTS, sizeof *wait_stacks);
        recent = calloc(RECENT, sizeof *recent);
        MemoryInfo info; u32 page;
        if (slots && R_SUCCEEDED(svcQueryMemory(&info, &page, (uintptr_t)switch_profile_thread)))
            module_base = info.addr;   /* the .nro's code region starts at the module's base */
        /* 0x2B: one step above the game's threads (0x2C), the same as SDL's audio thread */
        if (module_base && R_SUCCEEDED(threadCreate(&sampler, sampler_main, NULL, NULL, 0x10000, 0x2B, -2)) &&
            R_SUCCEEDED(threadStart(&sampler))) {
            sampler_on = 1;
            started_tick = armGetSystemTick();
            fprintf(stderr, "switch profile: sampling the named threads every ~1 ms (DHD_SWITCH_PROFILE)\n");
        } else fprintf(stderr, "switch profile: could not start\n");
    }
    if (sampler_on && thread_count < PROFILE_THREADS) {
        threads[thread_count].name = name;
        threads[thread_count].handle = threadGetCurHandle();
        MemoryInfo stack; u32 page;
        uintptr_t here = (uintptr_t)&stack;
        threads[thread_count].stack_top = R_SUCCEEDED(svcQueryMemory(&stack, &page, here)) ? stack.addr + stack.size : 0;
        __atomic_store_n(&thread_count, thread_count + 1, __ATOMIC_RELEASE);
    }
    mutexUnlock(&register_lock);
}

static void write_slots(FILE *f)
{
    for (uint32_t i = 0; i < PROFILE_SLOTS; i++) {
        if (!slots[i].key) continue;
        uint64_t key = slots[i].key;
        fprintf(f, "pair %s 0x%llx 0x%llx %u\n", threads[key & 7].name, (unsigned long long)(key >> 31 & OUTSIDE),
                (unsigned long long)(key >> 3 & OUTSIDE), slots[i].count);
    }
}

/* At exit (switch_main.c), before the threads end: stop sampling and write profile.txt. */
void switch_profile_stop(const char *dir)
{
    if (!sampler_on) return;
    sampler_quit = 1;
    threadWaitForExit(&sampler);
    threadClose(&sampler);
    sampler_on = 0;
    stopped_tick = armGetSystemTick();
    char path[1100];
    snprintf(path, sizeof path, "%sprofile.txt", dir);
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "switch profile: cannot write %s\n", path); return; }
    double seconds = (double)(stopped_tick - started_tick) / armGetSystemTickFreq();
    fprintf(f, "# dhd switch profile: %.1f s, %llu ticks, %llu dropped; offsets from the module base 0x%llx\n",
            seconds, (unsigned long long)ticks, (unsigned long long)dropped, (unsigned long long)module_base);
    for (int t = 0; t < thread_count; t++)
        fprintf(f, "thread %s %llu %llu\n", threads[t].name, (unsigned long long)threads[t].samples,
                (unsigned long long)threads[t].failed);
    write_slots(f);
    for (unsigned w = 0; w < window_count; w++) {
        fprintf(f, "window %u %.1f %u\n", w + 1, windows[w].ms, windows[w].samples);
        for (uint32_t i = 0; i < WINDOW_SLOTS; i++) {
            const WindowSlot *e = &windows[w].slots[i];
            if (e->count)
                fprintf(f, "ws %u %s 0x%x 0x%x 0x%x 0x%x %u\n", w + 1, threads[e->key.thread].name, e->key.pc, e->key.lr,
                        e->key.up1, e->key.up2, e->count);
        }
    }
    for (uint32_t i = 0; wait_stacks && i < WAIT_SLOTS; i++) {
        const WaitStack *w = &wait_stacks[i];
        if (!w->count) continue;
        fprintf(f, "wait %s %u", threads[w->thread].name, w->count);
        for (int d = 0; d < WAIT_DEPTH; d++) fprintf(f, " 0x%x", w->at[d]);
        fputc('\n', f);
    }
    fclose(f);
    fprintf(stderr, "switch profile: %.1f s, %llu ticks written to %s\n", seconds, (unsigned long long)ticks, path);
}
