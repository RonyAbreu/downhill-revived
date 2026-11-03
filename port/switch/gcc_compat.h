/* Forced into every unit of the Switch build (build_host64.py --target switch-arm64), which
   compiles with devkitA64's GCC instead of clang: the clang builtins the shared code uses. */
#ifndef DHD_SWITCH_GCC_COMPAT_H
#define DHD_SWITCH_GCC_COMPAT_H
#if !defined(__clang__)
#ifndef __builtin_assume
#define __builtin_assume(cond) do { if (!(cond)) __builtin_unreachable(); } while (0)
#endif
#endif
#endif
