# 170 — The Switch's exit: what a program must give back before the Homebrew Menu's turn

Phase 4, NX4 (finding 144, "EXIT GAME's crash"). On the Switch, EXIT GAME returns to the Homebrew
Menu (sphaira), which hbloader loads into the **same process**, and from there the player may
start dhd.nro again, in that process too. Nothing of ours is cleaned up by a process end, so
two things have to be given back by the program itself:

- its **threads**: one left behind wakes inside the next program;
- its **heap**: hbloader hands every program it runs the same heap (`envGetHeapOverrideAddr`),
  so any page of it that this program leaves held by the kernel or a system service is
  inaccessible to the next one, which faults on its first write there.

## Threads: joined, not merely ended

The exit's census (`switch_thread_census`, `port/switch/clocks.c`: `svcGetThreadList` on the
loader's process handle) read `threads left at the return: game #942 native` after the earlier
fixes.

libnx implements pthreads over its own `Thread` (`nx/source/runtime/newlib.c`). `pthread_join`
is `threadWaitForExit` then `threadClose`, which closes the kernel handle, unmaps the stack's
mirror (`svcUnmapMemory`) and frees the stack. `pthread_detach` is not implemented
(`__syscall_thread_detach` is commented out as "Unsupported"). So a thread nobody joins -- every
`SDL_DetachThread`ed thread, SDL3's Switch build running its threads on pthreads -- keeps its
kernel thread object (the census lists it until the handle is closed) and its stack's mirror
mapping, whose heap source stays locked.

`game` and `native` had returned but were never joined. `#942` was the freeze watch
(`port/hle/host.c`): stopped at the exit but detached, and unnamed.

| thread | started by | at the exit |
|---|---|---|
| main (`sdl`) | the loader | the one returning to the loader |
| `game` | `sdl_host_main` (SDL thread) | `switch_quit` ends it (`threadExit`); the main loop joins it before `exit(0)` |
| `vu`, `gs` | graphics_queue.c, gs_queue.inc | joined by `graphics_shutdown` (an exit handler), as before |
| `native` (render) | `native_thread_start` | kept instead of detached on the Switch; `gs_native_halt` quits and joins it |
| `texpack` x2 | `texpack_open` | kept; `texpack_halt` (new) quits and joins them |
| `job` | `host_thread_detach` (disc check, lobby list) | kept on the Switch; joined at the next job or by `host_jobs_join` |
| `freeze watch` | host.c | named; kept, stopped and joined by `host_freeze_watch_stop` |
| `movie` | movie.c | joined when the movie ends, as before |
| `clocks` | clocks.c | joined by `switch_clocks_stop`, **before** `SDL_Quit` (after it, every `SDL_Thread` is invalid and `SDL_WaitThread` returns without joining: the census listed `clocks` once) |
| profiler sampler | profile.c (`threadCreate`) | joined by `switch_profile_stop`, as before |
| SDL's audio device | SDL | `SDL_Quit` |
| `watchdog` | `DHD_DUMP` only | a debugging aid, left detached |

The census's name table is a ring of 64 (the movie thread is named again at every movie, and
16 slots filled up over a session). Measured: `threads left at the return: none`.

## The heap: the GPU's transfer memory

With the threads gone, the exit itself was clean, but starting dhd.nro again from the menu
crashed (Atmosphère: User Break in hbloader, symbolized against dhd.elf): SDL creating the window
-> EGL -> Mesa's `nouveau_device_new` -> `nvInitialize` -> `tmemCreate`, zeroing its 8 MiB transfer
memory. The dhd.log of one of them (written by our exception handler) gave the fault: a memset of
0x800000 bytes faulting at its first page.

Our exit never closed SDL's video. The render thread ended with its GL context still current,
`SDL_Quit` never ran, so EGL, Mesa's nouveau device and nvdrv stayed open, and nvdrv kept the
previous run's transfer memory (created `Perm_None`) at the same heap address the next run's
allocation then landed on. Now:

- the render thread makes its context current nowhere as it ends (`native_thread_release_gl`;
  EGL keeps a context bound to a thread through `eglTerminate`);
- the exit calls `SDL_Quit` once the render thread has ended (`eglTerminate`, the nouveau
  device, `nvExit`), and closes any nvdrv session still open (logged; none was);
- the guest's `svcMapMemory` mappings in the stack region (guest_memory_switch.c, when the
  guest's address space lands there) are undone, since their heap sources are held the same way
  (`host64_switch_memory_release`).

Measured: EXIT GAME back to the menu with no crash.

## The heap: still held at the next start (a thread after all)

A second start from the menu then crashed one step earlier, at the top of `main`:
`socketInitialize` -> `bsdInitialize` -> `tmemCreate`, again zeroing a transfer memory. So pages
of the heap are still held after the exit. The crash report does not carry the fault address and
dhd.log is not open yet at that point, so the crash itself does not say what holds them. libnx's `bsdExit`
frees its transfer memory without waiting for the service to give it back (`tmemClose`; nvdrv's
cleanup waits, `tmemWaitForPermission`), and services torn down by libnx after our last exit
handler are outside what the exit can see.

The likely holder is our own: that build's census read `threads left at the return: clocks`.
The clock keeper is an SDL thread, joined by `switch_clocks_stop`, which that build ran after
`SDL_Quit`; `SDL_Quit` leaves every `SDL_Thread` invalid, so `SDL_WaitThread` returned without
joining and the keeper lived on into the next program, its libnx stack still mapped from heap
pages that stayed locked as the mapping's source. The keeper is now joined before `SDL_Quit`.

And the start now protects itself and the log says what is held:

- `__libnx_initheap` (guest_memory_switch.c), before anything allocates, scans the heap with
  `svcQueryMemory` and lists every block that is not plain read-write heap with no attribute
  (type, permission, attribute: borrowed for transfer memory, IPC- or device-mapped), then keeps
  the guest's span and malloc to the largest clear run. dhd.log names the blocks and how much
  was left out;
- the exit logs the same scan after every teardown (`host64_switch_heap_report`), which shows
  what this program itself leaves held.

Measured with both (the keeper joined first, the scan): three to four rounds of a race, EXIT GAME
and a start again from the Homebrew Menu, with no crash and no new crash report. The last start's
dhd.log lists no held heap block at all, so the scan had nothing to avoid there: the keeper's
join is what fixed it, and the scan stays as a guard and a diagnosis. (That run was ended
without EXIT GAME, so its exit scan is not in the log.)

## Other ways to start

A forwarder (an installed title that starts dhd.nro) ends its own process when the program
returns, so Horizon reclaims every thread and every page and none of the above can show there;
it also gives a game's memory without holding R and returns to the HOME menu. Worth naming in
the docs as the cleaner way, but starting from the Homebrew Menu is the usual one and has to
work, so the exit gives everything back regardless.

## Sources

libnx (`nx/source/runtime/newlib.c` pthreads, `kernel/thread.c`, `kernel/tmem.c`,
`services/nv.c`, `services/bsd.c`), devkitPro's SDL3 Switch branch (`src/thread/SDL_thread.c`,
`src/thread/pthread`), Atmosphère's crash reports.
