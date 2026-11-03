/* Guest (EE) memory for host-side code (docs/findings/075-portable-core.md, phase 2).

   A guest address is a 32-bit PS2 address as the module sees it: PS2 RAM at PS2_RAM_BASE
   (port/runtime/memory.h), the scratchpad, the VU memories and the hardware registers at their
   own addresses (0x11008000, 0x12001000, ...). Host code reaches guest memory only through
   these two macros; it never casts a guest address to a pointer or a pointer to a guest address
   by hand.

   In this build (32-bit Windows) the host pointer of a guest address is the address itself, so
   both macros are the identity. GUEST_BASE is the one hook point: on a 64-bit host it is the
   runtime variable guest_base (defined in port/host/host_win32.c), the host address of guest
   address 0 (GUEST(a) = base + a, GADDR(p) = p - base), and every host file changes with it
   unchanged. It stays 0 (the identity) until the 64-bit host maps guest memory itself. On i686
   it is the constant 0, so that build's code does not change. */
#ifndef DHD_GUEST_H
#define DHD_GUEST_H

#include <stdint.h>

#if defined(__x86_64__) || defined(_WIN64) || defined(__aarch64__)
/* Host address of guest address 0: a variable on a 64-bit host (host_win32.c). */
extern uintptr_t guest_base;
#define GUEST_BASE guest_base
#else
/* Host address of guest address 0: 0 here (identity). */
#define GUEST_BASE ((uintptr_t)0)
#endif

#ifdef __SWITCH__
/* The Switch has one flat guest block and no aliased RAM views (host64/guest_memory_switch.c):
   a RAM mirror address folds onto canonical RAM here, with memory_access.h's rules (the full
   views at 0x28000000 and 0x38000000; the low views from 4 MiB into 0x00000000, 0x20000000 and
   0x30000000). */
static inline uint32_t guest_fold(uint32_t a)
{
    if ((a & ~0x11ffffffu) == 0x28000000u) a &= ~0x30000000u;
    if ((a & 0x1ffffffu) >= 0x400000u &&
        (a < 0x02000000u || (a >= 0x20400000u && a < 0x22000000u) || (a >= 0x30400000u && a < 0x32000000u)))
        a = 0x08000000u + (a & 0x1ffffffu);
    return a;
}
#define GUEST(addr) ((void *)(GUEST_BASE + (uintptr_t)guest_fold((uint32_t)(addr))))
#else
/* Host pointer of a 32-bit guest address. */
#define GUEST(addr) ((void *)(GUEST_BASE + (uintptr_t)(uint32_t)(addr)))
#endif

/* Guest address of a host pointer (or of a host address held in an integer). The result is
   32 bits: on a 64-bit host it is only meaningful for pointers that point into guest memory. */
#define GADDR(ptr) ((uint32_t)((uintptr_t)(ptr) - GUEST_BASE))

#endif
