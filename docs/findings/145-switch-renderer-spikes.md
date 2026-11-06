# 145 — The Switch renderer: where the long fields come from, and the pieces it lacked

Phase 4, NX2/NX3 (finding 144). The Switch's GL renderer (`port/switch/render_native.c`,
`textures.c`) averaged 4 ms a field on hardware, yet single fields reached 232 ms in races and
346 ms in menus, the audio underran around them, and STATE.md listed four renderer features as
missing. This is what the measurements said and what was done. All numbers are from a real
Switch (Tegra X1, Mesa 20.1 nouveau, OpenGL 4.3 core), scale 2, clocks at `max`.

## Measuring a long field

`render_native.c` now times every cycle of the render thread, from one picture's start to the
next one's: the picture (its draws, with any program it linked, any texture it uploaded, any
surface it made, the copies and conversions, the dead textures, the scanout), the present
draw, the pacing wait to the picture's due time, the swap (`render_gl.c` records its start and
end), and what is left, which is the wait for the GS worker to hand over the next field. A cycle
over `DHD_SLOW_FIELD_MS` (50 by default, 0 off) logs one line, ending in what took most:

    gs native: slow field 229.3 ms: picture 0.4 (...), present 0.3, pacing 15.0, swap 0.6,
      ..., waiting for the next field 213.1 -> the CPU threads (waiting for the next field)

Three clock reads a picture; nothing is printed while fields are short.

## What the long fields were

Three hardware runs (menus, then 2-5 minute races on the same track):

- **The longest fields are not the renderer's.** Every field of 124-230 ms (the menus' 239-242 ms
  and the race's 175-230 ms) had a picture under 1 ms and the render thread waiting 124-213 ms for
  the next field. Those are the game, VU or GS threads (the CPU profile is separate work; the
  perf lines put the limit on `cpu:game` and `cpu:vu` throughout).
- **The renderer's were texture uploads at the start of a race.** About fifteen fields of
  50-80 ms each uploaded 20-90 decoded textures (0.5-10 MiB), 0.6-0.8 ms apiece; the rest of
  the picture was slower in step. In steady racing the render thread is 3.8-4.7 ms a field.
- **No program was linked inside a field** once the programs were linked at start-up (below);
  before, each new kind of draw linked one there, a full compile of both stages on nouveau.

## What was changed

- **Programs linked at start-up.** Every draw program variant (the `PROGRAM_*` keys a picture
  can ask for, 24 of them) is linked and drawn with once into a 1 x 1 scissor, plus the Z probe's,
  the alpha upload's and the conversion's programs: 27 programs in 1.56 s, during the boot
  logos. Mesa can leave part of a compile to the first draw, hence the draw. `DHD_SWITCH_PRELINK
  = 0` links on first use, for comparison.
- **Texture objects pooled.** A released texture's GL object goes to a pool (64 MiB at most)
  and the next upload of the same kind and size takes it. Measured: it reuses nothing at the
  start of a race (every texture there is new), so it does not touch the spikes above; it
  saves allocation in the menus' turnover and costs nothing.
- **An upload ring.** The pixels go through one 8 MiB pixel unpack buffer, mapped
  unsynchronised in two fenced halves, rather than the driver's own staging per
  `glTexSubImage2D` (`DHD_SWITCH_UPLOAD = direct` turns it off). The data cost per MiB went from
  about 9.5 ms to 5-14 ms depending on the field: no clear gain, and it held the one bug of
  this work. The first version worked the half out from the write offset, which rounding to
  256 bytes can leave exactly at the buffer's end; the half was then 2, the fence was stored past
  its array over the pool's count, and the pool search read far outside its array (two
  crashes at a race's start, symbolised from Atmosphère's report against `dhd.elf`: an abort in
  `dhdr_tex_gl`). The half is now a variable of its own; the next run had no crash.

The fields left over 60 ms from the renderer are those uploads: four in a five-minute session
(60, 61, 95 and 102 ms), each 2-10 MiB of new textures at a race's load. A draw cannot run
without its texture, so these cannot be spread over later fields as they are. What would remove
them is uploading on another thread with a shared GL context (EGL allows it, SDL's
`SDL_GL_SHARE_WITH_CURRENT_CONTEXT`), so the GS worker's decode is followed by the upload there
and the render thread only waits on a fence. Not done.

## The pieces STATE.md listed as missing

Translated from the Rust/WGSL renderer (`render.rs`, `surfaces.rs`, `zread.rs`, `convert.rs`,
`native.wgsl`), which stays the reference:

- **The Z probe (finding 097).** After a picture a compute pass copies the probed patch (at
  most 64 x 64 GS pixels, the texel at each scaled block's centre) of the Z surface into one of
  three buffers behind a fence; a later picture takes the newest finished one without waiting.
  A Z surface now has a nearest, single-level filter so it can be fetched at all.
- **Reversed triangle order** for the alpha-only second draw of a GREATER-with-writes draw
  blending on As: the copies go after the picture's own vertices (or indices) in the same
  buffer, as `arenas.rs` does.
- **A draw sampling its own target** draws from a copy of the region its texture can reach, in
  the scratch texture at the same coordinates.
- **`ALPHA_UPLOAD`** (`native_upload_alpha`): a scissored pass over a copy of the page,
  `(old & ~mask) | bits`.
- **Conversions** (`native_convert`) as a GLSL compute shader, the page arithmetic with shifts
  where WGSL divides by 64 or 128 (this GPU has no integer divide); the converted texture is
  written bottom up and sampled like a surface.
- **A bug found on the way:** sampling another surface read its rows upside down. A surface
  holds the GS picture bottom up (the vertex stage's flip), and the surface path fetched as if
  it did not. The player saw particles, smoke and power-ups drawn wrong; after this change they
  report them drawn properly.

## Blending without dual-source

`blend_approx` counts the GS blends `native_blend` cannot express. The log now names each mode
once a run (`gs native: blend approximated: ...`); a whole session of menus and races named one:
`(Cd - 0) * As + Cd`, the destination times 1 + As/128. It was reduced to Zero, One (the draw
did nothing). `native_core.inc` marks it with flag 4096, and the Switch's shader writes As/128
as the colour and blends DstColor, One: Cd * As/128 + Cd, exact for As up to 128 (above, Cd * 2,
where the output stops at 1). `blend_approx` still counts it, since the count is made where the
key is built. The Rust renderer does not read the flag and keeps the approximation; native.wgsl
could do the same thing.

## Numbers, same track and rider

| | before (main build) | after (`dhd-gpu`) |
| --- | --- | --- |
| race, session | 41.6 fields/s, max 236.4 ms | 43.3 fields/s, max 190.7 ms |
| race, 30 s windows after the first | max 36-236 ms | max 36-73 ms |
| render thread, race | 4.0 ms a field | 4.6 ms a field |
| renderer fields over 60 ms | (not measured) | 4 in 288 s, all at a race's load |

The main build of that comparison already had other CPU-side changes (`graphics_queue.c`), so
the frame rate difference is not the renderer's alone; the longest fields in both are the CPU
threads'.
