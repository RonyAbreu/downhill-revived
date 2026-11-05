# 144: Nintendo Switch homebrew port plan (task NX0)

Goal: a Switch homebrew build (`.nro`, devkitPro devkitA64 + libnx, Atmosphère, started in title
takeover) made from the same portable core as every other target: the wasm2c output of
`build/wasm/dhd_core.wasm`, the host units, and a new Switch target in `build_host64.py`. Like
Android (finding 091), the player brings their own USA disc image; nothing of the game is in the
`.nro`.

## What the other targets give for free

- The core is plain C (wasm2c) plus `vu1_aot` blocks; the EE kernel's fibers already have an arm64
  switch (`fiber_arm64.c`, AAPCS64, no unwind info). Disc and file I/O go through `host_file_*` on
  fopen, so `sdmc:/` paths work.
- Android arm64 is the closest target: no launcher, paths set by the host's main
  (`android_main.c`), `net.c` already offers no online play there, presence already returns 0.

## What is different on Horizon

| Part | Other targets | Switch |
| --- | --- | --- |
| Window, audio, pads, threads | SDL3 3.4.18 | devkitPro's SDL fork, branch `switch-sdl-3.4` (unreleased; EGL/GLES video, audio, joystick, timer, filesystem). Fallback: `switch-sdl2` 2.28.5 behind a small shim |
| Renderer | Rust (`port/render`, wgpu) | **No Rust std on Horizon** (`aarch64-nintendo-switch-freestanding` is no_std; wgpu needs std). A C backend for the `dhdr_*` ABI (`dhd_render.h`, `dhd_render_ext.h`) is needed |
| GPU API | DX12 / Vulkan / Metal | Vulkan 1.3 from Mesa's NVK built for Horizon (static `libvulkan.a`, as nfsu1-sw / defjam-sw ship), or desktop GL 4.3 from `switch-mesa` 20.1 |
| Shaders | WGSL compiled by wgpu at run time | WGSL translated offline (naga: SPIR-V for Vulkan, GLSL 430 for GL); `raster.wgsl` is compute with atomics and a storage image, `native.wgsl` vertex/fragment + compute, `native_dual.wgsl` dual-source blend (two-pass fallback exists) |
| `dhd.ini` reader, PNG decode | Rust crates (`dhd_settings`, `png`) | C replacements (the PC SETTINGS table is already generated C, `hle/settings_table.h`; PNG: a small decoder or stb_image) |
| Online, disc check for online, Discord | Rust (`dhd_net`, `dhd_disc`, `dhd_presence`) | Off, as on Android (`NET_OFFERED 0`) |
| Guest RAM | one 32 MiB memfd mapped six times (three full views, three low aliases), 8 GiB `PROT_NONE` reservation, commits by mprotect, guard pages instead of bounds checks, SIGSEGV reporter, deferred GS reads by page faults | No shared-memory aliasing for a homebrew. Plan: one RAM view; the module side already folds non-crossing alias accesses (`memory_access.h`); make the host side's `GUEST()` fold too; wasm2c bounds checks (`WASM_RT_MEMCHECK_BOUNDS_CHECK`) instead of guard pages; `DHD_DEFER_READS=0` (synchronous GS reads); faults reported through `__libnx_exception_handler` |
| Process glue | `/proc/self/exe`, dladdr, pipes for the log, signals, `pthread_getcpuclockid` | Stubs: exe path = the `.nro`'s folder on `sdmc:`, no code-range lookup, log written by the host to `dhd.log` directly |
| Clocks | the OS's | Raise CPU/GPU/EMC through `clkrst` (pcv before 8.0.0) and restore on exit, reapplied after dock/HOME/sleep; defer to sys-clk when it runs (as nfsu1-sw / defjam-sw) |

## Performance (the real risk)

Single-thread, an overclocked Switch (Cortex-A57 at 1785 MHz) is about 0.6x the Unisoc T612 of
finding 126/127 (36-40 fields/s at 1x, 55 after its pacing work); at the stock 1020 MHz about
0.35x. The game's threads (game, vu, gs, native render) spread over the four A57 cores. Expect well
under 60 fields/s at first; the work list is the one of findings 107/112/127, measured on hardware.

## Milestones

1. **NX1, core on Switch, no picture.** `--target switch-arm64` builds an `.nro` with the wasm2c core,
   the host units, SDL3 (devkitPro's fork) for input/audio/threads, a renderer stub that presents
   nothing, the C `dhd.ini` reader. Success: the game reaches the title (logged), sound plays, and
   `DHD_GPU_TIMES`-style counters give fields/s on hardware.
2. **NX2, picture.** A C `dhdr_*` backend, first the exact path's presentation (`dhdr_present_exact`
   for movies and the CPU scanout) so menus show, then the native path.
   - Step 1 (done): `port/switch/render_gl.c` is the device, the surface and presentation on an
     OpenGL 4.3 core context (Mesa nouveau over SDL's EGL window, devkitPro's glad);
     `render_native.c` is the native renderer's GL half, `native.wgsl`'s vertex transform and
     untextured pixel path as GLSL, one FBO per GS surface, a compute scanout and the present
     quad. Every draw flat-coloured.
   - Step 2 (done): textures. `port/switch/textures.c` is `DhdrTexture` — a decoded texture as
     w x h R32UI (each texel its RGBA8 bytes, as textures.rs), a replacement as RGBA8 with mip
     levels and the eight samplers of `shared.rs` (bilinear | U clamped << 1 | V clamped << 2).
     `dhd_render.h` lets create/retain/release run on any thread but there is one GL context on
     one thread, so a texture is a CPU copy until the render thread first samples it, and one
     released anywhere goes on a spin-locked dead list that `dhdr_tex_collect` drains between
     pictures. The fragment shader gained `texel_xy`, `wrap_coordinate`, `texel_bytes`,
     `fetch_u`, `replacement` and `combine`'s TFX block from `native.wgsl`; GL has no
     specialisation constants, so `native_spec` is always 0 (the generic path: `texture_sized()`
     false, so `bilinear_u`'s one-gather fast path is never taken) and the two pipelines — texels
     and sampled — are two compilations of one source behind `#define TEXELS`. `gl_FragCoord` is
     declared `origin_upper_left` to match WGSL's `@builtin(position)`. The scale word is built
     as `render.rs` builds it (`big_t | 1 << 8 | min(1, big_t) << 16`: only decoded textures and
     replacements are sampled here).
   - Step 3 (open): the depth test, blending (no dual-source blending on this device, so the
     two-pass fallback), the texture copies (`DHDR_CMD_COPY`, `IMPORT`, `ALPHA_UPLOAD`),
     sampling a surface or a conversion, and the Z probe of finding 097. A draw that would
     sample a surface still comes out flat.
   - Also open: PNG (`dhdr_png_size`, `dhdr_png_decode` are still refusals in
     `render_stubs.c`), so texture packs do not load and `button_icons.c` shows no button
     icons. The sharp font is not a PNG and does work.
3. **NX3, speed.** Clocks, profiling on hardware, the Android findings' optimisations, the Switch's
   own (NEON paths, thread placement).
4. **NX4, packaging.** `package_release.py --platform switch`: the `.nro`, a `dhd.ini` template, the
   players' text; the disc image goes beside it (`sdmc:/switch/downhill-revived/`).

## Sources

- devkitPro pacman packages for Switch: switch-sdl2 2.28.5, switch-mesa 20.1.0, libnx 4.12, deko3d.
- devkitPro/SDL branch `switch-sdl-3.4`.
- Vulkan on Switch: danfromtico/mesa-switch (Mesa with a Horizon backend, NVK), as built by
  antoxa2584x/nfsu1-sw, nfsu2-sw and R-drg/defjam-sw (static `libvulkan.a`, glslang at run time,
  `NVK_I_WANT_A_BROKEN_VULKAN_DRIVER`, EGL stubbed so only one Mesa is linked).
- libnx: `svcSetMemoryPermission` (heap only, `---`/`r--`/`rw-`), `__libnx_exception_handler`,
  `clkrst`/`pcv`.
