/* backtrace() and backtrace_symbols_fd() for the POSIX hosts' crash reports. glibc has them in
   <execinfo.h>; Android's libc only from API 33, and the Android build targets API 30 (finding 091),
   so there they are made here from the unwinder (_Unwind_Backtrace) and dladdr: each frame is
   printed as library+offset, which llvm-addr2line -e libmain.so turns into a source line.
   Async-signal-safe enough for a report on the way out (no malloc). */
#ifndef DHD_BACKTRACE_POSIX_H
#define DHD_BACKTRACE_POSIX_H
#if defined(__ANDROID__) && __ANDROID_API__ < 33
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unwind.h>

typedef struct { void **frames; int count, max; } DhdBacktrace;

static _Unwind_Reason_Code dhd_backtrace_step(struct _Unwind_Context *context, void *arg)
{
    DhdBacktrace *b = (DhdBacktrace *)arg;
    uintptr_t pc = _Unwind_GetIP(context);
    if (pc && b->count < b->max) b->frames[b->count++] = (void *)pc;
    return b->count < b->max ? _URC_NO_REASON : _URC_END_OF_STACK;
}

static inline int backtrace(void **frames, int max)
{
    DhdBacktrace b = {frames, 0, max};
    _Unwind_Backtrace(dhd_backtrace_step, &b);
    return b.count;
}

static inline void backtrace_symbols_fd(void *const *frames, int count, int fd)
{
    for (int i = 0; i < count; i++) {
        Dl_info info;
        uintptr_t pc = (uintptr_t)frames[i];
        if (dladdr(frames[i], &info) && info.dli_fname) {
            const char *name = strrchr(info.dli_fname, '/');
            dprintf(fd, "  #%02d %s+0x%llx%s%s\n", i, name ? name + 1 : info.dli_fname,
                    (unsigned long long)(pc - (uintptr_t)info.dli_fbase), info.dli_sname ? " " : "",
                    info.dli_sname ? info.dli_sname : "");
        } else {
            dprintf(fd, "  #%02d 0x%llx\n", i, (unsigned long long)pc);
        }
    }
}
#elif defined(__SWITCH__)
/* newlib has no <execinfo.h> nor dladdr: the same unwinder walk, each frame printed as an offset
   from __start__ (where libnx's crt0 loads the NRO), which aarch64-none-elf-addr2line -e dhd.elf
   turns into a source line. */
#include <stdint.h>
#include <stdio.h>
#include <unwind.h>

extern char __start__[];

typedef struct { void **frames; int count, max; } DhdBacktrace;

static _Unwind_Reason_Code dhd_backtrace_step(struct _Unwind_Context *context, void *arg)
{
    DhdBacktrace *b = (DhdBacktrace *)arg;
    uintptr_t pc = _Unwind_GetIP(context);
    if (pc && b->count < b->max) b->frames[b->count++] = (void *)pc;
    return b->count < b->max ? _URC_NO_REASON : _URC_END_OF_STACK;
}

static inline int backtrace(void **frames, int max)
{
    DhdBacktrace b = {frames, 0, max};
    _Unwind_Backtrace(dhd_backtrace_step, &b);
    return b.count;
}

static inline void backtrace_symbols_fd(void *const *frames, int count, int fd)
{
    (void)fd;  /* stderr is the only place a report goes on the Switch */
    for (int i = 0; i < count; i++)
        fprintf(stderr, "  #%02d dhd.elf+0x%llx\n", i,
                (unsigned long long)((uintptr_t)frames[i] - (uintptr_t)__start__));
}
#else
#include <execinfo.h>
#endif
#endif
