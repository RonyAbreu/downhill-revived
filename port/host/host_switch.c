/* The Nintendo Switch host of host_api.h (finding 167): host_posix.c's counterpart on Horizon with
   libnx. What differs: the program's path is the .nro's folder on the SD card (the homebrew
   loader passes it as argv[0]; switch_main.c keeps it), there is no page protection of guest RAM
   and no access-fault guard (graphics.c then reads GS store-image data synchronously, as with
   DHD_DEFER_READS=0), the code range and memory regions are unknown (their users are opt-in
   diagnostics), the cycle counter is the system tick, and the heartbeat is a libnx-backed
   pthread. Everything else (files, clock, threads, display) is host_sdl.c's, on SDL3. */
#include "host_api.h"
#include "host_os.h"
#include "../include/guest.h"

#include <switch.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---- environment, errors and the program ---- */
unsigned long host_last_error(void) { return (unsigned long)errno; }

const char *switch_exe_path(void);  /* switch_main.c: argv[0] or the default folder's dhd.nro */

unsigned host_exe_path(char *out, unsigned size)
{
    int k = snprintf(out, size, "%s", switch_exe_path());
    if (k <= 0 || (unsigned)k >= size) {
        if (size) out[0] = 0;
        return 0;
    }
    return (unsigned)k;
}

void host_exit(int code)
{
    void host_log_drain(void);  /* port/hle/host.c: the last lines reach dhd.log */
    host_log_drain();
    void switch_shutdown(void);  /* switch_main.c: clocks back, services closed */
    switch_shutdown();
    exit(code);
}

/* MXCSR spoken by the callers (vu0_interp.c, ps2asm.c), translated to arm64's FPCR exactly as in
   host_posix.c: RC <-> RMode, FTZ/DAZ <-> FZ, every exception masked. */
static const unsigned rc_to_rmode[4] = {0, 2, 1, 3}, rmode_to_rc[4] = {0, 2, 1, 3};

unsigned host_fp_csr(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, fpcr" : "=r"(v));
    return 0x1F80u | rmode_to_rc[(v >> 22) & 3] << 13 | ((v >> 24) & 1 ? 0x8040u : 0);
}

void host_fp_set_csr(unsigned csr)
{
    uint64_t now, v = (uint64_t)rc_to_rmode[(csr >> 13) & 3] << 22 | (csr & 0x8040u ? 1ull << 24 : 0);
    __asm__ volatile("mrs %0, fpcr" : "=r"(now));
    if (now != v) __asm__ volatile("msr fpcr, %0\n\tisb" :: "r"(v) : "memory");
}

/* Nanoseconds, like the other arm64 hosts (the system tick runs at 19.2 MHz). */
uint64_t host_cycles(void) { return armTicksToNs(armGetSystemTick()); }

void host_putenv(const char *text)
{
    const char *eq = strchr(text, '=');
    char name[256];
    if (!eq || eq == text || (size_t)(eq - text) >= sizeof name) return;
    memcpy(name, text, (size_t)(eq - text));
    name[eq - text] = 0;
    host_putenv_s(name, eq + 1);
}

void host_putenv_s(const char *name, const char *value)
{
    if (!value || !*value) unsetenv(name);
    else setenv(name, value, 1);
}

/* ---- memory protection and access faults ----
   Guest memory is committed by guest_memory_switch.c, with no page protection either, so the
   deferred store-image reads that need it are off (host_fault_guard refuses, graphics.c reads at
   once). */
int host_mem_protect(uint32_t at, uint32_t size, int protection)
{
    (void)at; (void)size; (void)protection;
    return 0;
}

int host_fault_guard(int (*on_fault)(uint32_t address))
{
    (void)on_fault;
    return 0;
}

int host_fault_guard_here(void) { return 0; }

/* Whether [p, p + n) is mapped readable (host.c's crash report and DHD_PEEK): the kernel's map,
   since there are no signals to catch a probe's fault with. */
int switch_readable(const void *p, size_t n)
{
    uintptr_t at = (uintptr_t)p, end = at + n;
    while (at < end) {
        MemoryInfo info;
        u32 page;
        if (R_FAILED(svcQueryMemory(&info, &page, at)) || info.type == MemType_Unmapped ||
            !(info.perm & Perm_R))
            return 0;
        at = info.addr + info.size;
    }
    return 1;
}

/* A CPU exception in the program: libnx calls this on its own stack (finding 167). host.c prints
   the crash report (dhd.log); returning leaves the exception unhandled, so Horizon ends the
   program (and Atmosphere writes its own report). */
u8 __nx_exception_stack[0x8000] __attribute__((aligned(16)));
u64 __nx_exception_stack_size = sizeof __nx_exception_stack;

void __libnx_exception_handler(ThreadExceptionDump *ctx)
{
    void host_switch_crashed(const char *name, uintptr_t address, const void *ctx);
    struct { unsigned long long regs[31], sp, pc; } c;  /* host.c's CONTEXT */
    const char *name;
    for (int i = 0; i < 29; i++) c.regs[i] = ctx->cpu_gprs[i].x;
    c.regs[29] = ctx->fp.x;
    c.regs[30] = ctx->lr.x;
    c.sp = ctx->sp.x;
    c.pc = ctx->pc.x;
    switch (ctx->error_desc) {
    case ThreadExceptionDesc_InstructionAbort: name = "instruction abort"; break;
    case ThreadExceptionDesc_MisalignedPC: name = "misaligned pc"; break;
    case ThreadExceptionDesc_MisalignedSP: name = "misaligned sp"; break;
    case ThreadExceptionDesc_SError: name = "SError"; break;
    case ThreadExceptionDesc_BadSVC: name = "bad svc"; break;
    case ThreadExceptionDesc_Trap: name = "trap"; break;
    default: name = "fault"; break;  /* a data abort, among others (ThreadExceptionDesc_Other) */
    }
    host_switch_crashed(name, (uintptr_t)ctx->far.x, &c);
    void host_log_drain(void);
    host_log_drain();
}

void host_code_range(uintptr_t *lo, uintptr_t *hi) { *lo = *hi = 0; }

void host_memory_regions(void (*visit)(uintptr_t base, uintptr_t end, int host_memory)) { (void)visit; }

/* ---- the heartbeat (autopad.c): a host thread calls beat() every 30 s ---- */
static void (*heartbeat_beat)(void);

static void *heartbeat_run(void *unused)
{
    (void)unused;
    for (;;) {
        svcSleepThread(30000000000ll);
        heartbeat_beat();
    }
    return NULL;
}

void host_heartbeat_start(void (*beat)(void))
{
    pthread_t t;
    heartbeat_beat = beat;
    if (pthread_create(&t, NULL, heartbeat_run, NULL) == 0) pthread_detach(t);
}

uintptr_t host_heartbeat_eip(void) { return 0; }

/* ---- the system (host_os.h) ---- */
void host_os_system(char *out, unsigned size)
{
    char version[32] = "?";
    SetSysFirmwareVersion fw;
    if (R_SUCCEEDED(setsysInitialize())) {
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) snprintf(version, sizeof version, "%s", fw.display_version);
        setsysExit();
    }
    snprintf(out, size, "Nintendo Switch, Horizon %s%s, Tegra X1 (aarch64)", version,
             hosversionIsAtmosphere() ? " (Atmosphere)" : "");
}

FILE *host_os_fopen(const char *path, const char *mode) { return fopen(path, mode); }
