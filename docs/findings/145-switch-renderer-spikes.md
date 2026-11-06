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

## The race-load spikes removed (second round)

The same track again, now with the profiler's windows: `DHD_SWITCH_PROFILE = 1` keeps the last
two seconds of samples, and each slow-field line names a window (`[profile window N]`) in which
`profile_report.py --windows` shows what every thread did during that field alone. The line
also gives its time since start and the uploads made while waiting.

### Why not an upload thread

The plan was a second EGL context sharing objects, on an upload thread, with fences. devkitPro's
Mesa (branch `switch-20.1.0-rc3`, `nvc0_context.c`) gives every context of a screen the screen's
one pushbuf, `nvc0->base.pushbuf = screen->base.pushbuf`, with no lock (upstream nouveau got
its multithreading fixes only years later). Two contexts used at once on two threads would write into one command
stream. So the uploads stay on the render thread, but are moved to where it has nothing to do.

### What an upload cost

The first round's build queued each new texture and uploaded it while the render thread waited
for the next field (`native_idle_uploads`, native_core.inc). That took the load screens' uploads
out of the pictures (54 textures, 10 MiB, inside a wait), but not the race start's: there the
fields come back to back and the thread never waits. The window of such a field (63 ms, 43
uploads) said where the time went: 65% of the render thread's samples were in nvservices IPC,
`nvAddressSpaceMap` (39%), `nvMapCreate` (11%), `UnmapBuffer` (8%), `nvMapClose`, against 10% in
`memcpy`. Each `glTexStorage2D` makes a buffer object, and each `glTexSubImage2D` makes a staging
one and frees it again (`nvc0_miptree_transfer_map`: `nouveau_bo_new` of a GART buffer, then an
M2MF copy). The pixel unpack ring did not avoid it: `st_TexSubImage` takes the blit path only when
the screen prefers it, which on this GPU without VRAM it does not, so it falls back to
`_mesa_store_texsubimage` and the same transfer.

### The atlas

`textures.c` now gives a decoded texture a slot of an atlas page: one R32UI texture holding up to
64 textures of one size (8 x 8 slots, a page at most 1024 texels a side), made the first time a
size needs one and kept while used (one empty page per size is kept too). Its texels are written
by a draw: the upload ring is a texture buffer (`glTexBuffer`, R32UI), and a fragment shader
fetches `base + y * width + x` from it into the slot's rectangle of the page, so no staging
object is made. The draws fetch at `texels_origin + xy`, conversions at `base_rect.xy + xy`; the
bounds are still the texture's own size, so nothing reads into a neighbour. The renderer's GL
state the copy touches (framebuffer, program, vertex array, viewport, blend, depth, scissor,
stencil, cull, colour mask) is saved and put back, since the copy also runs inside a picture.
Replacements (texture packs, the sharp font) keep a texture each. `DHD_SWITCH_UPLOAD` picks the
form: `atlas` (the default), `idle` (a texture each, uploaded while waiting), `render` (the ring
only) and `direct` (the first form).

### The CPU threads' long fields

The windows of the 187-243 ms fields at the boot, the menus and the race's load all said the same:
the GS worker ran `font_sharp_view` (55-67%) and `button_icons_paint` (20-23%) -- the sharp copy
of the game's font (finding 058) with the button icons painted in, about 1024 x 1024 texels of
B-spline magnification, 150-190 ms on an A57 -- while the game thread waited for it. The disc was
not the cause (the game thread's `fsFileRead` was a quarter of one such window, overlapped). On
the Switch (`FONT_SHARP_ASYNC`, font_sharp.h) the copy is now made on a thread of its own; the
font is drawn as the game has it until a later decode finds the copy made (a fraction of a
second, at a font's first appearance). Other targets keep making it in place.

### Numbers, same track

| | first round (idle uploads only) | atlas and font thread |
| --- | --- | --- |
| renderer fields over 50 ms | 4 (63-113 ms, race start) | none |
| 55 new textures, 10 MiB, at load | 59 ms (while waiting) | 11.7 ms |
| CPU threads' fields over 150 ms | 3 (195-243 ms) | none |
| longest field of the session | 243 ms | 91 ms |
| race | 40.8 fields/s, max 190 ms | 41.2 fields/s, max 75 ms |

The frame rate moves 39.6-47.7 fields/s between 30 s windows of the same race, so the sessions
do not separate on it; the limit stays `cpu:game` / `cpu:vu`.

### What is left

- At a race's load, one field of 90 ms where the GS worker decodes 10 MiB of new textures texel
  by texel (`native_decode` -> `gs_image_pixel` and `gs_pixel_address`, 73% of its samples). A
  decode by rows or by blocks, per format, instead of a full address computation per texel would
  shorten it; it is shared code (native_core.inc, memory.c).
- In a race, about one field in 30 s of 57-77 ms in which every sampled thread is mostly waiting
  (game 55-72%, VU 70-76%, GS 74-82%, render 93%). The windows do not show who holds them up; it
  is not the renderer, the disc or the font. The audio thread and the system's own threads are
  not sampled.
- The render thread writes the log itself (`fsFileWrite` was 11% of one window: the slow-field
  line), a small cost only while fields are slow.
