/* The Switch's CPU, GPU and memory clocks (finding 167, NX3).

   Horizon starts a homebrew at the handheld defaults: CPU 1020 MHz, GPU 307.2 MHz, EMC
   1331.2 MHz. The first measurement of the native renderer on hardware (DHD_GL_TIMES) put the
   whole field in the draws and tracked the pixel count almost exactly -- about 150 Mpixel/s of
   effective fill -- which is what a Tegra X1 gives at 307 MHz with a shader this long. The rates
   below are the ones Horizon itself uses for its own boost modes, not an overclock past stock.

   dhd.ini (`DHD_` keys reach the environment whatever the settings table holds, rust_shims.c):

       DHD_SWITCH_CLOCKS = on     CPU 1785, GPU 768, EMC 1600 MHz (the default)
       DHD_SWITCH_CLOCKS = max    CPU 1785, GPU 921.6, EMC 1600 MHz
       DHD_SWITCH_CLOCKS = off    leave every clock alone

   Raised clocks mean more heat and a shorter battery; `off` is also what to use when sys-clk is
   managing this title, so the two do not write over each other.

   The rates are set again every two seconds from a thread of its own, because docking, the HOME
   menu and sleep all put them back, and the original rates are restored at exit. */
#include <switch.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "clocks_switch.h"

typedef struct Clock {
    PcvModuleId module;        /* clkrst, 8.0.0 and later */
    PcvModule legacy;          /* pcv, before 8.0.0 */
    const char *name;
    uint32_t want, was;
    int open;
    ClkrstSession session;
} Clock;

static Clock g_clocks[] = {
    { PcvModuleId_CpuBus, PcvModule_CpuBus, "CPU", 0, 0, 0, {0} },
    { PcvModuleId_GPU,    PcvModule_GPU,    "GPU", 0, 0, 0, {0} },
    { PcvModuleId_EMC,    PcvModule_EMC,    "EMC", 0, 0, 0, {0} },
};
enum { CLOCK_COUNT = (int)(sizeof g_clocks / sizeof g_clocks[0]) };

static int g_legacy;           /* before 8.0.0: pcv rather than clkrst */
static int g_started;
static SDL_AtomicInt g_stop;
static SDL_Thread *g_keeper;

static int clock_get(Clock *c, uint32_t *hz)
{
    if (g_legacy) return R_SUCCEEDED(pcvGetClockRate(c->legacy, hz));
    return c->open && R_SUCCEEDED(clkrstGetClockRate(&c->session, hz));
}

static int clock_set(Clock *c, uint32_t hz)
{
    if (g_legacy) return R_SUCCEEDED(pcvSetClockRate(c->legacy, hz));
    return c->open && R_SUCCEEDED(clkrstSetClockRate(&c->session, hz));
}

/* Docking, the HOME menu and sleep all put the clocks back, and there is no one event for the
   three, so they are simply written again every two seconds. */
static int keep_clocks(void *unused)
{
    (void)unused;
    while (!SDL_GetAtomicInt(&g_stop)) {
        for (int i = 0; i < 20 && !SDL_GetAtomicInt(&g_stop); i++) SDL_Delay(100);
        if (SDL_GetAtomicInt(&g_stop)) break;
        for (int i = 0; i < CLOCK_COUNT; i++) {
            uint32_t now = 0;
            if (g_clocks[i].want && clock_get(&g_clocks[i], &now) && now != g_clocks[i].want)
                clock_set(&g_clocks[i], g_clocks[i].want);
        }
    }
    return 0;
}

void switch_clocks_start(const char *mode)
{
    if (g_started) return;
    if (!mode || !*mode) mode = "on";
    if (!strcasecmp(mode, "off") || !strcmp(mode, "0")) {
        fprintf(stderr, "switch: clocks left as Horizon set them (DHD_SWITCH_CLOCKS = %s)\n", mode);
        return;
    }
    int most = !strcasecmp(mode, "max");
    g_clocks[0].want = 1785000000u;                        /* CPU: Horizon's own boost rate */
    g_clocks[1].want = most ? 921600000u : 768000000u;     /* GPU: 921.6 is the boost one, 768 docked */
    g_clocks[2].want = 1600000000u;                        /* EMC: Horizon's own boost rate */

    g_legacy = hosversionBefore(8, 0, 0);
    if (g_legacy) {
        if (R_FAILED(pcvInitialize())) {
            fprintf(stderr, "switch: no pcv: the clocks stay as they are\n");
            return;
        }
    } else {
        if (R_FAILED(clkrstInitialize())) {
            fprintf(stderr, "switch: no clkrst: the clocks stay as they are\n");
            return;
        }
        for (int i = 0; i < CLOCK_COUNT; i++)
            g_clocks[i].open = R_SUCCEEDED(clkrstOpenSession(&g_clocks[i].session, g_clocks[i].module, 3));
    }
    g_started = 1;
    for (int i = 0; i < CLOCK_COUNT; i++) {
        Clock *c = &g_clocks[i];
        if (!clock_get(c, &c->was)) c->was = 0;
        if (!clock_set(c, c->want)) {
            fprintf(stderr, "switch: the %s clock would not take %u MHz (it stays at %u MHz)\n",
                    c->name, c->want / 1000000u, c->was / 1000000u);
            c->want = 0;   /* nothing to keep, and nothing to restore */
            continue;
        }
        uint32_t now = 0;
        clock_get(c, &now);
        fprintf(stderr, "switch: %s clock %u -> %u MHz\n", c->name, c->was / 1000000u,
                (now ? now : c->want) / 1000000u);
    }
    SDL_SetAtomicInt(&g_stop, 0);
    g_keeper = SDL_CreateThread(keep_clocks, "dhd-clocks", NULL);
}

void switch_clocks_stop(void)
{
    if (!g_started) return;
    g_started = 0;
    SDL_SetAtomicInt(&g_stop, 1);
    if (g_keeper) { SDL_WaitThread(g_keeper, NULL); g_keeper = NULL; }
    for (int i = 0; i < CLOCK_COUNT; i++) {
        Clock *c = &g_clocks[i];
        if (c->want && c->was) clock_set(c, c->was);
        if (c->open) { clkrstCloseSession(&c->session); c->open = 0; }
    }
    if (g_legacy) pcvExit(); else clkrstExit();
    fprintf(stderr, "switch: the clocks are back as they were\n");
}

/* ---- which core each of the port's threads runs on (finding 167, NX3) ----

   Horizon starts every thread on the process's default core, so the game, VU, GS and render
   threads queue behind one another on one of the three an application may use. The measurement
   that prompted this, scale 1 with the clocks raised: a field took 21.4 ms while the threads'
   own busy times were 5.3 (game), 18.2 (vu), 12.1 (gs) and 3.3 (render) -- more work than one
   core could have done, so some spreading was happening, but 3 ms more than the heaviest thread
   alone, which is what queueing costs.

   OFF BY DEFAULT, and the measurements are why. Three builds of the same scene at scale 2 --
   no pinning, then one layout, then another -- gave 24.2, 24.2 and 24.5 fields/s, so Horizon's
   own scheduler was already doing what a fixed map could. A race then showed why a fixed map
   cannot win: every thread is heavy there (a field's busy times: game 25-33 ms, vu 24-44, gs
   15-22, the native renderer 14-33), four of them over three cores, and which two are light
   enough to share changes with the scene and with the resolution. A scheduler free to move them
   beats a map chosen in advance.

   dhd.ini's DHD_SWITCH_CORES = on asks for the layout below anyway: game a core to itself, vu
   another, gs and the native renderer the third. It is kept for measuring, not because it is
   known to help. switch_main.c reads it from dhd.ini before the first thread is named (the
   launcher's own pass over dhd.ini comes later). With fewer than three cores nothing is pinned. */

static int core_list(int *cores, int max)
{
    uint64_t mask = 0;
    if (R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0))) return 0;
    int n = 0;
    for (int i = 0; i < 64 && n < max; i++)
        if (mask & (1ull << i)) cores[n++] = i;
    return n;
}

/* Every named thread's kernel id, for the census at exit (switch_thread_census). */
static struct { const char *name; u64 id; } named_threads[16];
static int named_count;

static void switch_thread_named(const char *name)
{
    u64 id;
    if (!name || R_FAILED(svcGetThreadId(&id, CUR_THREAD_HANDLE))) return;
    int i = __atomic_fetch_add(&named_count, 1, __ATOMIC_ACQ_REL);
    if (i < 16) { named_threads[i].name = name; named_threads[i].id = id; }
}

/* At the return to the loader (switch_main.c): which threads of this process are still there
   besides the caller. The Homebrew Menu is loaded into this same process next, so anything listed
   here is a thread that may wake inside it (finding 167). Waits up to ms for them to go, then
   names what is left: a named thread by its name, any other by its kernel id. Returns how many. */
int switch_thread_census(unsigned ms)
{
    u64 self = 0, ids[64];
    s32 count = 0;
    svcGetThreadId(&self, CUR_THREAD_HANDLE);
    /* The process's own handle, not the pseudo-handle (GetThreadList refuses pseudo-handles) nor
       0, which lists every thread on the console: the loader hands it over (envGetOwnProcessHandle) */
    Handle process = envGetOwnProcessHandle();
    if (process == CUR_PROCESS_HANDLE) {
        fprintf(stderr, "switch: no process handle from the loader: the threads left are not counted\n");
        return -1;
    }
    for (unsigned waited = 0;; waited += 10) {
        Result rc = svcGetThreadList(&count, ids, 64, process);
        if (R_FAILED(rc)) {
            fprintf(stderr, "switch: the thread list could not be read (0x%X)\n", (unsigned)rc);
            return -1;
        }
        int others = 0;
        for (s32 i = 0; i < count && i < 64; i++) others += ids[i] != self;
        if (!others || waited >= ms) break;
        svcSleepThread(10000000);
    }
    char line[512];
    int at = snprintf(line, sizeof line, "switch: threads left at the return:");
    int left = 0;
    for (s32 i = 0; i < count && i < 64 && at < (int)sizeof line - 32; i++) {
        if (ids[i] == self) continue;
        const char *name = NULL;
        for (int k = 0; k < named_count && k < 16; k++)
            if (named_threads[k].id == ids[i]) name = named_threads[k].name;
        at += name ? snprintf(line + at, sizeof line - at, " %s", name)
                   : snprintf(line + at, sizeof line - at, " #%llu", (unsigned long long)ids[i]);
        left++;
    }
    fprintf(stderr, "%s%s\n", line, left ? "" : " none");
    return left;
}

void switch_thread_core(const char *name)
{
    static int cores[8], count = -1, on;
    void switch_profile_thread(const char *name);   /* profile.c: DHD_SWITCH_PROFILE */
    switch_profile_thread(name);
    switch_thread_named(name);
    if (count < 0) {
        const char *want = getenv("DHD_SWITCH_CORES");
        on = want && (!strcasecmp(want, "on") || !strcmp(want, "1"));
        count = core_list(cores, 8);
        fprintf(stderr, "switch: %d core%s for this process; the threads %s\n", count,
                count == 1 ? "" : "s",
                on ? "are pinned (DHD_SWITCH_CORES = on)" : "stay where Horizon puts them");
    }
    if (!on || count < 3 || !name) return;
    int core;
    /* "native" is the native renderer's thread (native_core.inc). The perf lines call it
       "render", which is no thread's own name, and naming it that here was why it was the one
       thread the first attempt never moved.

       The layout follows what a race at scale 1 measures once the renderer stopped being the
       cost (finding 167, NX3): game 21-23 ms a field, vu 21-22, gs 13-16, native 3-4. The two
       heavy ones get a core each and the two light ones share the third, which is 17-20 ms
       against the 23 ms field. The first layout paired game with gs on one core, 37 ms of work,
       and was written when the renderer was the heaviest thread; that is why pinning measured
       as a wash. */
    if (!strcmp(name, "game")) core = cores[0];
    else if (!strcmp(name, "vu")) core = cores[1];
    else if (!strcmp(name, "gs") || !strcmp(name, "native")) core = cores[2];
    else return;   /* the main thread, SDL's audio, movie, texpack: left alone */
    if (R_FAILED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core))) {
        fprintf(stderr, "switch: the %s thread would not move to core %d\n", name, core);
        return;
    }
    fprintf(stderr, "switch: the %s thread runs on core %d\n", name, core);
}

/* SDL's audio thread, raised from inside the audio callback (sound_out_sdl.c): it is the one
   thread of the lot that must never be late, and Horizon starts it at the ordinary priority
   among four threads that want more than three cores. 0x2B is one step above the default 0x2C
   (a lower number is a higher priority on Horizon), the same step switch-hero gives its own
   audio and input threads. DHD_SWITCH_AUDIO_PRIORITY = off leaves it alone. */
void switch_audio_thread(void)
{
    const char *want = getenv("DHD_SWITCH_AUDIO_PRIORITY");
    if (want && (!strcasecmp(want, "off") || !strcmp(want, "0"))) return;
    int32_t prio = want && *want ? (int32_t)strtol(want, NULL, 0) : 0x2B;
    if (prio < 0x1C || prio > 0x3F) prio = 0x2B;   /* the range Horizon gives an application */
    if (R_FAILED(svcSetThreadPriority(CUR_THREAD_HANDLE, (u32)prio)))
        fprintf(stderr, "switch: the audio thread would not take priority 0x%X\n", (unsigned)prio);
    else
        fprintf(stderr, "switch: the audio thread runs at priority 0x%X\n", (unsigned)prio);
}
