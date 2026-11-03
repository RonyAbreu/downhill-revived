/* Horizon has no mmap: the Switch build's stand-in for the one header that names it without
   using it (WABT's wasm-rt-impl.c, whose signal handler and mmap-backed memory are off here:
   WASM_RT_SKIP_SIGNAL_RECOVERY, and the guest block is guest_memory_switch.c's). */
#ifndef DHD_SWITCH_SYS_MMAN_H
#define DHD_SWITCH_SYS_MMAN_H
#endif
