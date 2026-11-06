/* The Switch's native renderer, NX2 steps 1 and 2 (finding 144): textured GS geometry on
   OpenGL 4.3 core (DHD_RENDER=native; dhd.ini "renderer = native"). It stands in
   for the Rust renderer's native half (port/render/src/native, native.wgsl) the same way
   render_gl.c stands in for its device and presentation: the command loop, the draw pipeline, the
   scanout and the present pass, all translated line for line from native.wgsl and
   port/render/src/native/render.rs. The CPU half (arenas, surfaces as native_core.inc sees them,
   the picture it builds) stays portable C (native_core.inc, gpu_rust_native.inc,
   gpu_rust_shared.inc), unchanged on every target.

   What it draws: every DHDR_CMD_DRAW, with its vertex colour, fog, alpha test and — step 2 —
   its texture: a decoded texture (textures.c's R32UI words, fetched by the shader's texel path,
   native.wgsl's native_pixel_u) or a replacement, a texture pack's or the sharp font's (RGBA8
   mips through one of the eight samplers, native_pixel). Step 3 adds the depth test and
   blending, as pipelines.rs's key describes them: the Z surface is attached to the colour
   surface's framebuffer, and a draw that blends on As is split in two (the colour with As / 128
   as its source alpha, then the alpha channel alone, unblended), because this device offers no
   dual-source blending.

   Also here: a draw sampling a surface (another one, or its own target through a copy), the
   texture copies (DHDR_CMD_COPY, IMPORT, ALPHA_UPLOAD), the Z probe of finding 097, and the
   reversed triangle order the alpha pass of a GREATER-with-writes draw wants. Still missing: a
   draw sampling a conversion (convert), which loses its texture bit and comes out flat.

   Scanout and present are exact (native.wgsl's native_scanout, in a compute shader, and the same
   windowed quad render_gl.c's exact present uses), so the picture is in the right place and the
   right shape.

   GL context: render_gl.c's, made current lazily on the first dhdr_present_exact or
   dhdr_native_present call (render_gl_ext.h). The native renderer's own GL calls (building
   surfaces, drawing a picture) happen earlier in the same field (gpu_rust_shared.inc's
   native_render, then native_present), on the same thread, before any present call has run yet on
   the very first field: this file makes the game's one surface current itself, the same way
   render_gl.c's present calls do. */
#include <glad/glad.h>
#include <SDL3/SDL.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dhd_render.h"
#include "dhd_render_ext.h"
#include "render_gl_ext.h"
#include "textures_gl.h"

enum { DHDR_NATIVE_SURFACE_COUNT = 48 };
enum { ZREAD_MAX = 64, ZREAD_SLOTS = 3 };   /* zread.rs: GS pixels a side, buffers in flight */
enum { CONVERTS = 64 };                    /* convert.rs: conversions a picture holds at most */

typedef struct NativeSurface {
    GLuint texture, fbo;
    uint32_t width, height;    /* scaled pixels, as dhdr_native_surface's w, h */
    uint32_t scale;            /* the resolution scale this surface's texture is at */
    int depth;                 /* a depth surface (GL_DEPTH_COMPONENT32F): the Z of the draws into
                                  a colour surface, attached to that surface's framebuffer */
    GLuint depth_attached;     /* the depth texture on this colour surface's framebuffer, or 0 */
} NativeSurface;

/* The draw pipelines. The Rust renderer specialises its fragment stage with WebGPU's
   specialisation constants (native.wgsl's native_spec), so the paths a draw does not take cost
   it nothing; OpenGL has no such thing, and the first measurement on hardware (finding 144, step
   3) showed why that matters: with everything decided at run time, 57 draws of a 640 x 448
   picture took 107 ms, and the cost tracked the pixel count almost exactly (four times the
   pixels, about a quarter of the rate). So the variants are separate compilations of one source
   instead, keyed by PROGRAM_* below and linked the first time a draw asks for one.

   TEXELS: a decoded texture's R32UI words (native_pixel_u) rather than a replacement's sampled
   RGBA8 mips (native_pixel); an untextured draw takes a TEXELS program with the texture bit
   cleared. BILINEAR: the four-texel mix, which a point-sampled draw should not pay for.
   ALPHA_TEST: the `discard`, whose mere presence in the source makes a driver give up early
   depth rejection for the draws that have no alpha test at all. */
enum {
    PROGRAM_TEXELS = 1, PROGRAM_BILINEAR = 2, PROGRAM_ALPHA_TEST = 4, PROGRAM_SCALE_ONE = 8,
    PROGRAM_WRAP_REPEAT = 16,
    PROGRAM_COUNT = 32
};

typedef struct DrawProgram {
    GLuint id;
    int tried;                 /* linked, or tried and failed: never built again */
    GLint u_target_size, u_flags, u_scale, u_atst, u_aref, u_fog;
    GLint u_alpha_scale, u_texa_alpha, u_tex_offset, u_tex_size, u_wrap, u_region, u_alpha_div;
    GLint u_texels_size;       /* the GL texture's own size, so the shader never asks for it */
    /* What the program already holds. A GS picture is thousands of draws whose state mostly
       repeats, and a uniform the program already has is a driver call for nothing; `held` says
       the fields below are what was last sent to this program (uniforms are per program, so
       each has its own copy). */
    int held;
    float h_target[2], h_fog[4], h_alpha_scale, h_texa_alpha, h_alpha_div;
    uint32_t h_flags, h_scale, h_atst, h_aref, h_tex_size[2], h_wrap[2], h_region[4];
    int32_t h_tex_offset[2], h_texels_size[2];
} DrawProgram;

/* The scanout's forms (DHD_SWITCH_SCANOUT): a draw with the divisions in float (the default), the
   same draw with integer divisions, the compute shader, and none at all (the display texture
   keeps its last picture: for measuring what the scanout costs, not for play). */
enum { SCAN_DRAW, SCAN_INT, SCAN_COMPUTE, SCAN_NONE, SCAN_FORMS };
static const char *const scanout_names[SCAN_FORMS] = { "draw", "int", "compute", "none" };

struct DhdrNative {
    DrawProgram programs[PROGRAM_COUNT];
    /* The scanout's forms (scanout_mode): SCAN_DRAW, SCAN_INT, SCAN_COMPUTE. */
    struct { GLuint id; GLint da, db, ds; } scanout[3];
    int scanout_mode;          /* SCAN_*, from DHD_SWITCH_SCANOUT */
    int scanout_cycle;         /* DHD_SWITCH_SCANOUT = cycle: the next form at each times line */
    GLuint present_program;
    GLint u_rect;
    GLuint vao, vbo, ibo;
    size_t vbo_bytes, ibo_bytes;
    int ready;                 /* the GL objects above are built (ensure_gl_objects) */
    NativeSurface surfaces[DHDR_NATIVE_SURFACE_COUNT];
    GLuint display_texture, display_fbo;
    uint32_t display_width, display_height;
    /* A surface copying onto itself cannot read and write one texture at once, so it goes
       through this; an IMPORT's pixels land here first and are blitted on (surfaces.rs). */
    GLuint scratch_texture, scratch_fbo;
    uint32_t scratch_width, scratch_height;
    GLuint import_texture, import_fbo;
    uint32_t import_width, import_height;
    GLuint upload_program, upload_texture;   /* ALPHA_UPLOAD (alpha_upload) */
    GLint upload_map;
    /* Conversions (run_convert): the program, the inverse table, the constants block, and one
       converted texture per conversion index (convert.rs's converted[64]). */
    GLuint convert_program, convert_inverse, convert_constants;
    struct { GLuint texture; uint32_t w, h; } converted[CONVERTS];
    int point_filter;
    /* The Z probe (finding 097, zread.rs): a compute pass copies the probed patch of a Z surface
       into one of these buffers after a picture, and a later picture takes the newest one whose
       fence has passed, never waiting for one. */
    struct { GLuint buffer; GLsync fence; uint32_t rect[4]; uint64_t tag; } zslots[ZREAD_SLOTS];
    GLuint zread_program;
    GLint zread_rect, zread_scale;
};

/* ---- slow fields: what the render thread did in a field that took too long ----

   A field's cycle on the render thread, from one picture's start to the next one's: the picture
   (dhdr_native_picture_indexed: the draws, with any program it had to link, any texture it had to
   upload and any surface it had to make; the dead textures; the scanout), the present draw, the
   pacing wait until the picture's due time, the swap, and then the wait for the GS worker to hand
   over the next field. When a cycle is longer than DHD_SLOW_FIELD_MS (50 by default; 0 turns it
   off), one line names each part's time, so a spike says whether this thread caused it (links,
   uploads, surfaces, draws, the swap) or waited for the CPU threads that make the fields. */
typedef struct FieldCost {
    uint64_t start, end;          /* the picture's own start and end (SDL_GetTicksNS) */
    uint64_t draws_ns, collect_ns, scan_ns;
    uint32_t links;   uint64_t link_ns;
    uint32_t surfaces; uint64_t surface_ns;
    uint64_t uploads, upload_ns, upload_bytes;
    uint64_t present_began, present_ended;   /* dhdr_native_present, 0 when it did not run */
    uint32_t draws, copies, imports, converts;
} FieldCost;

/* g_cost sums what happens from one picture's end to the next one's (surfaces are made before
   the picture that draws into them, links and uploads inside it); at a picture's end it becomes
   g_last, the cycle the next picture's start checks. Render thread only. */
static FieldCost g_cost, g_last;
static uint64_t g_cost_fields;  /* pictures so far */

static int slow_field_ms(void)
{
    static int ms = -1;
    if (ms < 0) {   /* getenv, not SDL_getenv: dhd.ini reaches the environment after SDL's copy */
        const char *s = getenv("DHD_SLOW_FIELD_MS");
        ms = s ? atoi(s) : 50;
        if (ms < 0) ms = 0;
    }
    return ms;
}

/* At a picture's start: the last cycle, logged when it was slow. */
static void slow_field_check(uint64_t now)
{
    static unsigned printed;
    const FieldCost *c = &g_last, *next = &g_cost;   /* next: what ran since, before this picture */
    int limit = slow_field_ms();
    if (!limit || !c->start || g_cost_fields < 2) return;
    double ms = (now - c->start) / 1e6;
    if (ms < limit || printed >= 400) return;
    printed++;
    double picture = (c->end - c->start) / 1e6, present = 0, pace = 0, swap = 0, wait;
    uint64_t last = c->end;
    if (c->present_began >= c->end) {
        present = (c->present_ended - c->present_began) / 1e6;
        last = c->present_ended;
        if (dhdr_gl_swap_began_ns >= c->present_ended) {
            pace = (dhdr_gl_swap_began_ns - c->present_ended) / 1e6;
            swap = (dhdr_gl_swap_ended_ns - dhdr_gl_swap_began_ns) / 1e6;
            last = dhdr_gl_swap_ended_ns;
        }
    }
    /* Surfaces made for this picture ran after the last swap, outside both pictures. */
    double before = next->surface_ns / 1e6;
    wait = now > last ? (now - last) / 1e6 - before : 0;
    if (wait < 0) wait = 0;
    /* What the cycle went into most: this thread's own work, the pacing, or the wait for a field. */
    double own = picture + present + swap + before;
    const char *verdict = wait >= own && wait >= pace ? "the CPU threads (waiting for the next field)"
                        : pace >= own ? "pacing" : swap >= picture + present ? "the swap (GPU or display)"
                        : "the render thread";
    fprintf(stderr, "gs native: slow field %.1f ms: picture %.1f (draws %.1f over %u draws, %u links %.1f ms, "
                    "%llu uploads %.1f ms %.0f KiB, %u surfaces %.1f ms, %u copies, %u imports, %u conversions, "
                    "dead textures %.1f, "
                    "scanout %.1f), present %.1f, pacing %.1f, swap %.1f, %u surfaces made for the next %.1f, "
                    "waiting for the next field %.1f -> %s\n",
            ms, picture, c->draws_ns / 1e6, c->draws, c->links, c->link_ns / 1e6,
            (unsigned long long)c->uploads, c->upload_ns / 1e6, c->upload_bytes / 1024.0, c->surfaces,
            c->surface_ns / 1e6, c->copies, c->imports, c->converts, c->collect_ns / 1e6, c->scan_ns / 1e6, present, pace, swap,
            next->surfaces, before, wait, verdict);
}

/* ---- shaders (native.wgsl, translated line for line for the untextured step 1 subset) ---- */

/* native_vertex: the sprite-upscale snap (flag 1024, scale's low byte > 1) and the GS-pixel to
   clip-space map (nat.target_size, native.wgsl's comment: "GS x lands at x * T"). stqf is read (so
   fog, native.wgsl's i.stqf.w, keeps working) but texturing itself is step 2's. */
static const char *native_vertex_source =
    "#version 430 core\n"
    "layout(location = 0) in vec3 position_in;\n"
    "layout(location = 1) in vec4 stqf;\n"
    "layout(location = 2) in uvec4 color_in;\n"
    "out vec4 v_stqf;\n"
    "out vec4 v_color;\n"
    "uniform vec2 target_size;\n"
    "uniform uint flags;\n"
    "uniform uint nscale;\n"
    "void main() {\n"
    "    vec3 position = position_in;\n"
    "    if ((flags & 1024u) != 0u && (nscale & 255u) > 1u) {\n"
    "        position = vec3(ceil(position.xy - 0.5), position.z);\n"
    "    }\n"
    /* WebGPU's clip space has z in [0, 1] and OpenGL's in [-1, 1], and the GS Z the picture
       carries is WebGPU's. glClipControl would say so, but it is core only in 4.5, so the vertex
       stage maps it: the window depth GL works back to, 0.5 * (2z - 1) + 0.5, is z again. */
    "    gl_Position = vec4(position.xy / target_size * vec2(2.0, -2.0) + vec2(-1.0, 1.0),\n"
    "                       position.z * 2.0 - 1.0, 1.0);\n"
    "    v_stqf = stqf;\n"
    "    v_color = vec4(color_in);\n"
    "}\n";

/* native_pixel (and native_pixel_u with TEXELS): texture_uv, the texel fetch or the replacement
   sample, combine() and the output, the same arithmetic as native.wgsl. The uniform block is
   loose uniforms here. native_spec is always 0, so draw_flags() is `flags`, wrap_mode() is
   `wrap` and alpha_test() is `atst`; what the Rust path puts in specialisation constants is
   here either a uniform or one of the PROGRAM_* compilations. texture_sized() is effectively
   true: the host passes the GL texture's own size in `texels_size`, so the shader never calls
   textureSize (a driver query, four times over for a bilinear fetch). bilinear_u's one-gather
   fast path is still not taken; its four loads are.

   gl_FragCoord is declared origin_upper_left so that, with the vertex stage's Y flip, it counts
   GS pixels from the top exactly as WGSL's @builtin(position) does (fetch's flag-1024 snap).

   Sampling a surface or a conversion is still missing: the host clears the texture bit of a draw
   it cannot give a texture to, so such a draw comes out flat as it did in step 1. */
static const char *native_fragment_source =
    "layout(origin_upper_left) in vec4 gl_FragCoord;\n"
    "in vec4 v_stqf;\n"
    "in vec4 v_color;\n"
    "out vec4 frag_color;\n"
    "uniform uint flags;\n"
    "uniform uint atst;\n"
    "uniform uint aref;\n"
    "uniform vec4 fog;\n"
    "uniform uint nscale;\n"
    /* 255 for native_pixel, 128 for native_pixel_blend (the RGB pass of a draw that blends on As
       where there is no dual-source blending): the only difference between the two entry points,
       so a uniform rather than a third compilation. */
    "uniform float alpha_div;\n"
    "uniform float alpha_scale;\n"
    "uniform float texa_alpha;\n"
    "uniform ivec2 tex_offset;\n"
    "uniform uvec2 tex_size;\n"
    "uniform uvec2 wrap;\n"
    "uniform uvec4 region;\n"
    /* The sampled texture's own size, which the host knows: no textureSize anywhere, and a
       bilinear fetch would have asked for it four times over. */
    "uniform ivec2 texels_size;\n"
    "#ifdef TEXELS\n"
    "uniform usampler2D native_texels;\n"
    "#else\n"
    "uniform sampler2D native_texture;\n"
    "#endif\n"
    /* CLAMP modes: repeat, clamp, region clamp, region repeat (MINU the mask, MAXU the fix).
       WRAP_REPEAT is both axes on plain repeat, which most draws are: texel_xy calls this twice
       and a bilinear fetch calls texel_xy four times, so the three compares cost eight branches
       a pixel for a mode that never changes within a draw. */
    "#ifdef WRAP_REPEAT\n"
    "uint wrap_coordinate(int c, uint mode, uint size, uint low, uint high) {\n"
    "    return uint(c) & (size - 1u);\n"
    "}\n"
    "#else\n"
    "uint wrap_coordinate(int c, uint mode, uint size, uint low, uint high) {\n"
    "    if (mode == 0u) return uint(c) & (size - 1u);\n"
    "    if (mode == 1u) return uint(clamp(c, 0, int(size) - 1));\n"
    "    if (mode == 2u) return uint(clamp(c, int(low), int(high)));\n"
    "    return (uint(c) & low) | high;\n"
    "}\n"
    "#endif\n"
    /* The texture pixel holding GS texel c (S the texture's scale, E the draw's).

       SCALE_ONE is S == E == 1, which is every draw that reads a decoded texture or a
       replacement: render.rs's scale word only carries another S when the draw samples a
       surface. The general form below costs two integer divisions a texel, and a GPU has no
       integer divide -- each is a long sequence -- so a bilinear fetch was paying eight of them
       per pixel. With S and E one they all fold away: whole is c, sub is zero, stp is one. */
    "#ifdef SCALE_ONE\n"
    "ivec2 texel_xy(ivec2 c) {\n"
    "    uvec2 at = uvec2(wrap_coordinate(c.x, wrap.x, tex_size.x, region.x, region.y),\n"
    "                     wrap_coordinate(c.y, wrap.y, tex_size.y, region.z, region.w));\n"
    "    return tex_offset + ivec2(at);\n"
    "}\n"
    "#else\n"
    "ivec2 texel_xy(ivec2 c) {\n"
    "    uint S = (nscale >> 8) & 255u;\n"
    "    uint E = (nscale >> 16) & 255u;\n"
    "    uint stp = S / E;\n"
    "    ivec2 whole = ivec2(floor(vec2(c) / float(E)));\n"
    "    ivec2 sub = c - whole * int(E);\n"
    "    uvec2 at = uvec2(wrap_coordinate(whole.x, wrap.x, tex_size.x, region.x, region.y),\n"
    "                     wrap_coordinate(whole.y, wrap.y, tex_size.y, region.z, region.w));\n"
    "    return (tex_offset + ivec2(at)) * int(S) + sub * int(stp) + int(stp / 2u);\n"
    "}\n"
    "#endif\n"
    "#ifdef TEXELS\n"
    "uint load_texels(ivec2 xy) {\n"
    "    ivec2 size = texels_size;\n"
    "    uint w = texelFetch(native_texels, clamp(xy, ivec2(0), size - 1), 0).x;\n"
    "    return (all(greaterThanEqual(xy, ivec2(0))) && all(lessThan(xy, size))) ? w : 0u;\n"
    "}\n"
    /* A decoded texel's RGBA bytes in GS units (texel_bytes). */
    "vec4 texel_bytes(uint w) {\n"
    "    float a = float(w >> 24);\n"
    "    float alpha = (alpha_scale == 255.0) ? a : round(a / 255.0 * alpha_scale);\n"
    "    return vec4(float(w & 255u), float((w >> 8) & 255u), float((w >> 16) & 255u),\n"
    "                ((flags & 256u) != 0u) ? texa_alpha : alpha);\n"
    "}\n"
    "vec4 texel_u(ivec2 c) { return texel_bytes(load_texels(texel_xy(c))); }\n"
    "vec4 fetch_u(vec2 uv_in, vec2 across, vec2 down) {\n"
    "    vec2 uv = uv_in;\n"
    "    uint T = nscale & 255u;\n"
    "#ifdef SCALE_ONE\n"
    /* E is one, so its test holds and the multiply below is nothing. */
    "    if ((flags & 1024u) != 0u && T > 1u) {\n"
    "        vec2 corner = floor(gl_FragCoord.xy / float(T)) * float(T) - gl_FragCoord.xy;\n"
    "        uv += across * corner.x + down * corner.y;\n"
    "    }\n"
    "#else\n"
    "    uint E = (nscale >> 16) & 255u;\n"
    "    if ((flags & 1024u) != 0u && T > 1u && E == 1u) {\n"
    "        vec2 corner = floor(gl_FragCoord.xy / float(T)) * float(T) - gl_FragCoord.xy;\n"
    "        uv += across * corner.x + down * corner.y;\n"
    "    }\n"
    "    uv *= float(E);\n"
    "#endif\n"
    /* BILINEAR is the draw's flag 32, decided when the program was linked: a point-sampled draw
       does not carry the other three fetches, nor a branch over them. */
    "#ifdef BILINEAR\n"
    "    uv -= 0.5;\n"
    "    ivec2 b = ivec2(floor(uv));\n"
    "    vec2 f = uv - floor(uv);\n"
    "    return mix(mix(texel_u(b), texel_u(b + ivec2(1, 0)), f.x),\n"
    "               mix(texel_u(b + ivec2(0, 1)), texel_u(b + ivec2(1, 1)), f.x), f.y);\n"
    "#else\n"
    "    return texel_u(ivec2(floor(uv)));\n"
    "#endif\n"
    "}\n"
    "#else\n"
    /* A replacement (flag 2048): the mips sampled, region clamp applied half a texel inside. */
    "vec4 replacement(vec2 uv_in, vec2 across, vec2 down) {\n"
    "    vec2 uv = uv_in;\n"
    "    vec2 size = vec2(texels_size);\n"
    "    vec2 gs_size = vec2(tex_size);\n"
    "    vec2 half_texel = gs_size / size * 0.5;\n"
    "    if (wrap.x == 2u) uv.x = clamp(uv.x, float(region.x) + half_texel.x, float(region.y) + 1.0 - half_texel.x);\n"
    "    if (wrap.y == 2u) uv.y = clamp(uv.y, float(region.z) + half_texel.y, float(region.w) + 1.0 - half_texel.y);\n"
    "    vec4 t = textureGrad(native_texture, uv / gs_size, across / gs_size, down / gs_size);\n"
    "    return vec4(t.rgb * 255.0, t.a * alpha_scale);\n"
    "}\n"
    /* A surface this draw samples (native.wgsl's load_texel and texel): another colour surface's
       RGBA8, read texel by texel with the same wrap and scale arithmetic as a decoded texture,
       and turned into GS units the same way. A loaded unorm8 times 255 is exactly the byte, and
       round() keeps it so where a compiler would fuse the product into a later add. */
    "vec4 load_texel_f(ivec2 xy) {\n"
    "    ivec2 size = texels_size;\n"
    "    ivec2 c = clamp(xy, ivec2(0), size - 1);\n"
    /* A surface holds its GS rows bottom up (native_vertex's flip): GS row y is texture row
       size.y - 1 - y. texels_size is the surface's own size, also when the texture is the
       scratch copy of it (a draw sampling its own target), which keeps the same rows. */
    "    vec4 v = texelFetch(native_texture, ivec2(c.x, size.y - 1 - c.y), 0);\n"
    "    return (all(greaterThanEqual(xy, ivec2(0))) && all(lessThan(xy, size))) ? v : vec4(0.0);\n"
    "}\n"
    "vec4 texel(ivec2 c) {\n"
    "    vec4 t = load_texel_f(texel_xy(c));\n"
    "    return vec4(round(t.rgb * 255.0),\n"
    "                ((flags & 256u) != 0u) ? texa_alpha : round(t.a * alpha_scale));\n"
    "}\n"
    "vec4 fetch_surface(vec2 uv_in, vec2 across, vec2 down) {\n"
    "    vec2 uv = uv_in;\n"
    "    uint T = nscale & 255u;\n"
    "    uint E = (nscale >> 16) & 255u;\n"
    "    if ((flags & 1024u) != 0u && T > 1u && E == 1u) {\n"
    "        vec2 corner = floor(gl_FragCoord.xy / float(T)) * float(T) - gl_FragCoord.xy;\n"
    "        uv += across * corner.x + down * corner.y;\n"
    "    }\n"
    "    uv *= float(E);\n"
    "    if ((flags & 32u) != 0u) {\n"
    "        uv -= 0.5;\n"
    "        ivec2 b = ivec2(floor(uv));\n"
    "        vec2 f = uv - floor(uv);\n"
    "        return mix(mix(texel(b), texel(b + ivec2(1, 0)), f.x),\n"
    "                   mix(texel(b + ivec2(0, 1)), texel(b + ivec2(1, 1)), f.x), f.y);\n"
    "    }\n"
    "    return texel(ivec2(floor(uv)));\n"
    "}\n"
    "#endif\n"
    "vec4 combine(vec4 t) {\n"
    "    vec4 color = v_color;\n"
    "    if ((flags & 1u) != 0u) {\n"
    "        uint tfx = (flags >> 2) & 3u;\n"
    "        vec3 rgb = (tfx == 1u) ? t.rgb : min(floor(t.rgb * color.rgb / 128.0), vec3(255.0));\n"
    "        if (tfx >= 2u) rgb = min(rgb + color.a, vec3(255.0));\n"
    "        float a = color.a;\n"
    "        if ((flags & 2u) != 0u) {\n"
    "            if (tfx == 0u) a = min(floor(t.a * color.a / 128.0), 255.0);\n"
    "            else if (tfx == 2u) a = min(t.a + color.a, 255.0);\n"
    "            else a = t.a;\n"
    "        }\n"
    "        color = vec4(rgb, a);\n"
    "    }\n"
    "    if ((flags & 16u) != 0u) { color = vec4(floor(mix(fog.rgb, color.rgb, v_stqf.w)), color.a); }\n"
    /* ALPHA_TEST is the draw's flag 64. The `discard` is left out of the source entirely when
       the draw has no alpha test, because a driver that can see one anywhere in the stage gives
       up early depth rejection for every pixel, test or no test. */
    "#ifdef ALPHA_TEST\n"
    "    if ((flags & 64u) != 0u) {\n"
    "        uint a = uint(color.a);\n"
    "        bool passed = atst == 1u || (atst == 2u && a < aref) || (atst == 3u && a <= aref) ||\n"
    "                      (atst == 4u && a == aref) || (atst == 5u && a >= aref) || (atst == 6u && a > aref) ||\n"
    "                      (atst == 7u && a != aref);\n"
    "        if (passed == ((flags & 128u) != 0u)) discard;\n"
    "    }\n"
    "#endif\n"
    "    if ((flags & 512u) != 0u) { color = vec4(floor(color.rgb * color.a / 128.0), color.a); }\n"
    "    return color;\n"
    "}\n"
    "vec2 texture_uv() {\n"
    "    float q = (abs(v_stqf.z) > 1e-30) ? v_stqf.z : 1e-30;\n"
    "    return v_stqf.xy / q * vec2(tex_size);\n"
    "}\n"
    "void main() {\n"
    "    vec2 uv = texture_uv();\n"
    "    vec2 across = dFdx(uv);\n"
    "    vec2 down = dFdy(uv);\n"
    "    vec4 t = vec4(0.0);\n"
    "    if ((flags & 1u) != 0u) {\n"
    "#ifdef TEXELS\n"
    "        t = fetch_u(uv, across, down);\n"
    "#else\n"
    /* native.wgsl's fetch(): the float texture is a replacement or a surface. */
    "        t = ((flags & 2048u) != 0u) ? replacement(uv, across, down)\n"
    "                                    : fetch_surface(uv, across, down);\n"
    "#endif\n"
    "    }\n"
    "    vec4 color = combine(t);\n"
    "    frag_color = vec4(color.rgb / 255.0, clamp(color.a / alpha_div, 0.0, 1.0));\n"
    "}\n";

/* native_scanout (native.wgsl), the exact (non-"fast") form: da = display[0..3] (psm, bw, scale
   byte unused here, dbx), db = display[4..7] (dby, width, height, interlace), ds = display[8]
   (the resolution scale, set by this file as scanout() does in render.rs).

   The Y flip: native_vertex maps GS y = 0 onto clip y = +1, which is a render target's LAST
   texture row in OpenGL (row 0 is the bottom one), so a surface texture holds the GS picture
   upside down. WebGPU has no such difference and native.wgsl needs no flip; here the fetch
   turns the row round, so the display texture comes out with GS row 0 first, which is what the
   present pass and dhdr_native_read_finish expect. */
static const char *scanout_source =
    "#version 430 core\n"
    "layout(local_size_x = 8, local_size_y = 8) in;\n"
    "layout(binding = 0) uniform sampler2D source;\n"
    "layout(rgba8, binding = 1) uniform writeonly image2D screen;\n"
    "uniform uvec4 da;\n"
    "uniform uvec4 db;\n"
    "uniform uint ds;\n"
    "void main() {\n"
    "    uvec2 tid = gl_GlobalInvocationID.xy;\n"
    "    if (tid.x >= db.y || tid.y >= db.z) return;\n"
    "    uvec2 at = uvec2(tid.x, tid.y >> db.w);\n"
    "    uvec2 origin = uvec2(da.w, db.x);\n"
    "    uvec2 xy = ((origin + at / ds) & uvec2(2047u)) * ds + at % ds;\n"
    "    ivec2 size = textureSize(source, 0);\n"
    "    vec3 c = (int(xy.x) < size.x && int(xy.y) < size.y)\n"
    "           ? texelFetch(source, ivec2(int(xy.x), size.y - 1 - int(xy.y)), 0).rgb : vec3(0.0);\n"
    "    imageStore(screen, ivec2(tid), vec4(c, 1.0));\n"
    "}\n";

/* The same scanout as a draw: one triangle over the display texture's framebuffer, the fragment's
   pixel standing for the compute invocation's id (framebuffer row 0 is texture row 0, so the
   display texture comes out exactly as the compute form writes it).

   On the Switch (Mesa's nouveau, Tegra X1) the compute form measured 24 ms of GPU a picture at
   1024x896 and 30 ms at 1280x896 (DHD_GL_TIMES = 2, finding 144): a cost in proportion to the
   pixels. The same shader as a draw measured the same 24 and 30 ms, so the stage was not it.
   What is left per pixel is `at / ds` and `at % ds`: four integer divisions, and this GPU has no
   integer divide -- the same cost texel_xy had in the draws (107 ms to 1.2 ms once
   PROGRAM_SCALE_ONE folded its divisions away, STATE.md NX3). So the quotient is taken in float:
   (at + 0.5) / ds is never within 1/16 of a whole number for a whole `at` and ds <= 8, far
   wider than a float's error at these sizes, so its floor is the exact quotient, and the
   remainder follows from it.

   SCANOUT_INT_DIV compiles the integer form, for DHD_SWITCH_SCANOUT = int and cycle
   (scanout_mode below), which measure the forms against one another on hardware. */
static const char *scanout_vertex_source =
    "#version 430 core\n"
    "void main() {\n"
    "    vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
static const char *scanout_fragment_source =   /* after a prefix carrying #version */
    "layout(binding = 0) uniform sampler2D source;\n"
    "uniform uvec4 da;\n"
    "uniform uvec4 db;\n"
    "uniform uint ds;\n"
    "out vec4 color;\n"
    "void main() {\n"
    "    uvec2 tid = uvec2(gl_FragCoord.xy);\n"
    "    uvec2 at = uvec2(tid.x, tid.y >> db.w);\n"
    "    uvec2 origin = uvec2(da.w, db.x);\n"
    "#ifdef SCANOUT_INT_DIV\n"
    "    uvec2 q = at / ds, r = at % ds;\n"
    "#else\n"
    "    uvec2 q = uvec2(floor((vec2(at) + 0.5) / float(ds)));\n"
    "    uvec2 r = at - q * ds;\n"
    "#endif\n"
    "    uvec2 xy = ((origin + q) & uvec2(2047u)) * ds + r;\n"
    "    ivec2 size = textureSize(source, 0);\n"
    "    vec3 c = (int(xy.x) < size.x && int(xy.y) < size.y)\n"
    "           ? texelFetch(source, ivec2(int(xy.x), size.y - 1 - int(xy.y)), 0).rgb : vec3(0.0);\n"
    "    color = vec4(c, 1.0);\n"
    "}\n";

/* The present quad, as render_gl.c's exact present: a triangle strip from gl_VertexID, rect's xy
   the clip-space top-left corner and zw the bottom-right one. */
static const char *present_vertex_source =
    "#version 430 core\n"
    "uniform vec4 rect;\n"
    "out vec2 uv;\n"
    "void main() {\n"
    "    vec2 corner = vec2(gl_VertexID & 1, gl_VertexID >> 1);\n"
    "    uv = corner;\n"
    "    gl_Position = vec4(mix(rect.xy, rect.zw, corner), 0.0, 1.0);\n"
    "}\n";
static const char *present_fragment_source =
    "#version 430 core\n"
    "uniform sampler2D picture;\n"
    "in vec2 uv;\n"
    "out vec4 color;\n"
    "void main() { color = vec4(texture(picture, uv).rgb, 1.0); }\n";

/* `prefix`, when given, is prepended: the `#version` line and the fragment stage's `#define`
   (GL has no specialisation constants, so the two draw pipelines are two compilations). */
static GLuint compile(GLenum kind, const char *prefix, const char *source)
{
    GLuint s = glCreateShader(kind);
    GLint ok = 0;
    const char *parts[2] = { prefix, source };
    glShaderSource(s, prefix ? 2 : 1, prefix ? parts : &parts[1], NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = "";
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "gs native: OpenGL shader: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

/* A program from one or two shaders (compute: just cs); fs_prefix heads the fragment source.
   0 on failure (logged). */
static GLuint link_program(const char *vs_src, const char *fs_prefix, const char *fs_src, const char *cs_src)
{
    GLuint vs = 0, fs = 0, cs = 0, program;
    if (cs_src) {
        if (!(cs = compile(GL_COMPUTE_SHADER, NULL, cs_src))) return 0;
    } else {
        if (!(vs = compile(GL_VERTEX_SHADER, NULL, vs_src)) ||
            !(fs = compile(GL_FRAGMENT_SHADER, fs_prefix, fs_src))) {
            glDeleteShader(vs);
            glDeleteShader(fs);
            return 0;
        }
    }
    program = glCreateProgram();
    if (cs) glAttachShader(program, cs);
    else { glAttachShader(program, vs); glAttachShader(program, fs); }
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glDeleteShader(cs);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[1024] = "";
        glGetProgramInfoLog(program, sizeof log, NULL, log);
        fprintf(stderr, "gs native: OpenGL program did not link: %s\n", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

/* This single-window game's one surface, made current (render_gl_ext.h). 0: no surface yet, or no
   OpenGL (render_gl.c has already logged why); every caller then does nothing. */
static int gl_ready(void)
{
    DhdrSurface *s = dhdr_gl_active_surface();
    return s && dhdr_gl_make_current(s);
}

/* ---- creation ---- (dhdr_native_create runs before any window/GL context exists; ensure_gl_objects builds the
   programs and buffers the first time a GL call finds one ready) */

/* The two draw pipelines' uniform locations (the same names in both). */
static int find_draw_uniforms(DrawProgram *d, const char *sampler_name, GLint unit)
{
    if (!d->id) return 0;
    d->u_texels_size = glGetUniformLocation(d->id, "texels_size");
    d->u_target_size = glGetUniformLocation(d->id, "target_size");
    d->u_flags = glGetUniformLocation(d->id, "flags");
    d->u_scale = glGetUniformLocation(d->id, "nscale");
    d->u_atst = glGetUniformLocation(d->id, "atst");
    d->u_aref = glGetUniformLocation(d->id, "aref");
    d->u_fog = glGetUniformLocation(d->id, "fog");
    d->u_alpha_scale = glGetUniformLocation(d->id, "alpha_scale");
    d->u_texa_alpha = glGetUniformLocation(d->id, "texa_alpha");
    d->u_tex_offset = glGetUniformLocation(d->id, "tex_offset");
    d->u_tex_size = glGetUniformLocation(d->id, "tex_size");
    d->u_wrap = glGetUniformLocation(d->id, "wrap");
    d->u_region = glGetUniformLocation(d->id, "region");
    d->u_alpha_div = glGetUniformLocation(d->id, "alpha_div");
    glUseProgram(d->id);
    glUniform1i(glGetUniformLocation(d->id, sampler_name), unit);
    return 1;
}

/* The PROGRAM_* variant, linked the first time a draw asks for it (a picture uses a handful of
   the sixteen, and linking them all at the start would cost seconds of startup for nothing).
   NULL when it could not be built; the caller then skips the draw. */
static DrawProgram *draw_program(DhdrNative *n, unsigned key)
{
    DrawProgram *d = &n->programs[key & (PROGRAM_COUNT - 1)];
    if (d->tried) return d->id ? d : NULL;
    d->tried = 1;
    char defines[200];
    snprintf(defines, sizeof defines, "#version 430 core\n%s%s%s%s%s",
             (key & PROGRAM_TEXELS) ? "#define TEXELS 1\n" : "",
             (key & PROGRAM_BILINEAR) ? "#define BILINEAR 1\n" : "",
             (key & PROGRAM_ALPHA_TEST) ? "#define ALPHA_TEST 1\n" : "",
             (key & PROGRAM_SCALE_ONE) ? "#define SCALE_ONE 1\n" : "",
             (key & PROGRAM_WRAP_REPEAT) ? "#define WRAP_REPEAT 1\n" : "");
    uint64_t began = SDL_GetTicksNS();
    d->id = link_program(native_vertex_source, defines, native_fragment_source, NULL);
    g_cost.links++;
    g_cost.link_ns += SDL_GetTicksNS() - began;
    if (!d->id) return NULL;
    find_draw_uniforms(d, (key & PROGRAM_TEXELS) ? "native_texels" : "native_texture", 0);
    return d;
}

static int ensure_aux(GLuint *texture, GLuint *fbo, uint32_t *have_w, uint32_t *have_h,
                      uint32_t w, uint32_t h);
static void ensure_buffer(GLenum target, GLuint buffer, size_t *capacity, size_t bytes);
static int ensure_zread(DhdrNative *n);
static int ensure_upload(DhdrNative *n);
static int ensure_convert(DhdrNative *n);

/* Every draw program, linked and drawn with once, before the first picture.

   Linking one on first use (as this file first did) put each link inside the field that first
   needed it, and on Mesa's nouveau a link is a full compile of both stages -- tens of
   milliseconds on this CPU. A race or a menu reaching a new kind of draw then stalled a field.
   Mesa may also leave part of the work for the first draw (its state tracker picks a variant
   when a draw validates the program), so each program draws one pixel into the scratch
   framebuffer and the queue is finished, all at start-up. DHD_SWITCH_PRELINK = 0 goes back to
   linking on first use (to measure the difference). */
static void prelink_programs(DhdrNative *n)
{
    const char *env = getenv("DHD_SWITCH_PRELINK");
    if (env && !strcmp(env, "0")) return;
    uint64_t began = SDL_GetTicksNS();
    if (!ensure_aux(&n->scratch_texture, &n->scratch_fbo, &n->scratch_width, &n->scratch_height, 4, 4)) return;
    DhdrVertex corner[3];
    memset(corner, 0, sizeof corner);
    corner[0].x = 0; corner[0].y = 0;
    corner[1].x = 4; corner[1].y = 0;
    corner[2].x = 0; corner[2].y = 4;
    for (int i = 0; i < 3; i++) { corner[i].q = 1; corner[i].rgba = 0x80808080u; }
    glBindVertexArray(n->vao);
    ensure_buffer(GL_ARRAY_BUFFER, n->vbo, &n->vbo_bytes, sizeof corner);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof corner, corner);
    glBindFramebuffer(GL_FRAMEBUFFER, n->scratch_fbo);
    glViewport(0, 0, 4, 4);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 1, 1);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glActiveTexture(GL_TEXTURE0);
    unsigned linked = 0;
    for (unsigned key = 0; key < PROGRAM_COUNT; key++) {
        if ((key & PROGRAM_BILINEAR) && !(key & PROGRAM_TEXELS)) continue;   /* never asked for */
        DrawProgram *d = draw_program(n, key);
        if (!d) continue;
        linked++;
        glUseProgram(d->id);
        glBindTexture(GL_TEXTURE_2D, (key & PROGRAM_TEXELS) ? dhdr_tex_dummy_texels() : dhdr_tex_dummy_rgba());
        /* Uniforms sent here bypass the held copies (d->held stays 0), so the first real draw
           sends its own. */
        glUniform2f(d->u_target_size, 4, 4);
        glUniform1ui(d->u_flags, 1);
        glUniform1ui(d->u_scale, 1 | 1 << 8 | 1 << 16);
        glUniform2i(d->u_texels_size, 1, 1);
        glUniform2ui(d->u_tex_size, 1, 1);
        glUniform1f(d->u_alpha_div, 255.0f);
        glUniform1f(d->u_alpha_scale, 255.0f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    /* The other programs a picture may need: the Z probe, the alpha upload, the conversions.
       (Each is used for a handful of pixels, so a first use costs only its link, done here.) */
    linked += ensure_zread(n) + ensure_upload(n) + ensure_convert(n);
    glFinish();
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    fprintf(stderr, "gs native: %u programs linked (draw programs drawn with) at start-up in %.1f ms\n", linked,
            (SDL_GetTicksNS() - began) / 1e6);
    /* Start-up links are not a field's: the first slow field line must not count them. */
    g_cost.links = 0; g_cost.link_ns = 0;
}

static int ensure_gl_objects(DhdrNative *n)
{
    if (n->ready) return 1;
    if (!gl_ready()) return 0;
    n->scanout[SCAN_DRAW].id = link_program(scanout_vertex_source, "#version 430 core\n", scanout_fragment_source, NULL);
    n->scanout[SCAN_INT].id = link_program(scanout_vertex_source, "#version 430 core\n#define SCANOUT_INT_DIV\n",
                                           scanout_fragment_source, NULL);
    n->scanout[SCAN_COMPUTE].id = link_program(NULL, NULL, NULL, scanout_source);
    n->present_program = link_program(present_vertex_source, NULL, present_fragment_source, NULL);
    if (!n->scanout[SCAN_DRAW].id || !n->scanout[SCAN_INT].id || !n->scanout[SCAN_COMPUTE].id ||
        !n->present_program)
        return 0;
    for (int i = 0; i < 3; i++) {
        n->scanout[i].da = glGetUniformLocation(n->scanout[i].id, "da");
        n->scanout[i].db = glGetUniformLocation(n->scanout[i].id, "db");
        n->scanout[i].ds = glGetUniformLocation(n->scanout[i].id, "ds");
    }
    {   /* getenv, not SDL_getenv: dhd.ini reaches the environment after SDL took its copy */
        const char *s = getenv("DHD_SWITCH_SCANOUT");
        n->scanout_mode = SCAN_DRAW;
        if (s && !strcmp(s, "int")) n->scanout_mode = SCAN_INT;
        else if (s && !strcmp(s, "compute")) n->scanout_mode = SCAN_COMPUTE;
        else if (s && !strcmp(s, "none")) n->scanout_mode = SCAN_NONE;
        else if (s && !strcmp(s, "cycle")) n->scanout_cycle = 1;
    }
    n->u_rect = glGetUniformLocation(n->present_program, "rect");
    glGenVertexArrays(1, &n->vao);
    glGenBuffers(1, &n->vbo);
    glGenBuffers(1, &n->ibo);
    glBindVertexArray(n->vao);
    glBindBuffer(GL_ARRAY_BUFFER, n->vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(DhdrVertex), (void *)offsetof(DhdrVertex, x));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(DhdrVertex), (void *)offsetof(DhdrVertex, s));
    glEnableVertexAttribArray(2);
    glVertexAttribIPointer(2, 4, GL_UNSIGNED_BYTE, sizeof(DhdrVertex), (void *)offsetof(DhdrVertex, rgba));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, n->ibo);
    prelink_programs(n);
    glBindVertexArray(n->vao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, n->ibo);
    fprintf(stderr, "gs native: GL native renderer ready (scanout: %s)\n",
            n->scanout_cycle ? "cycle, the next form at each DHD_GL_TIMES line" : scanout_names[n->scanout_mode]);
    n->ready = 1;
    return 1;
}

DhdrNative *dhdr_native_create(uint32_t flags)
{
    (void)flags;   /* DHDR_NATIVE_DUAL: dhdr_device_features never offers dual-source blending here */
    return calloc(1, sizeof(DhdrNative));
}

void dhdr_native_destroy(DhdrNative *n)
{
    if (!n) return;
    if (n->ready && gl_ready()) {
        for (int i = 0; i < DHDR_NATIVE_SURFACE_COUNT; i++) {
            if (n->surfaces[i].fbo) glDeleteFramebuffers(1, &n->surfaces[i].fbo);
            if (n->surfaces[i].texture) glDeleteTextures(1, &n->surfaces[i].texture);
        }
        if (n->display_fbo) glDeleteFramebuffers(1, &n->display_fbo);
        if (n->display_texture) glDeleteTextures(1, &n->display_texture);
        if (n->scratch_fbo) glDeleteFramebuffers(1, &n->scratch_fbo);
        if (n->scratch_texture) glDeleteTextures(1, &n->scratch_texture);
        if (n->import_fbo) glDeleteFramebuffers(1, &n->import_fbo);
        if (n->upload_texture) glDeleteTextures(1, &n->upload_texture);
        if (n->upload_program) glDeleteProgram(n->upload_program);
        for (int i = 0; i < CONVERTS; i++)
            if (n->converted[i].texture) glDeleteTextures(1, &n->converted[i].texture);
        if (n->convert_program) glDeleteProgram(n->convert_program);
        if (n->convert_inverse) glDeleteBuffers(1, &n->convert_inverse);
        if (n->convert_constants) glDeleteBuffers(1, &n->convert_constants);
        if (n->import_texture) glDeleteTextures(1, &n->import_texture);
        glDeleteBuffers(1, &n->vbo);
        glDeleteBuffers(1, &n->ibo);
        glDeleteVertexArrays(1, &n->vao);
        for (int i = 0; i < PROGRAM_COUNT; i++)
            if (n->programs[i].id) glDeleteProgram(n->programs[i].id);
        for (int i = 0; i < 3; i++) glDeleteProgram(n->scanout[i].id);
        for (int i = 0; i < ZREAD_SLOTS; i++) {
            if (n->zslots[i].fence) glDeleteSync(n->zslots[i].fence);
            if (n->zslots[i].buffer) glDeleteBuffers(1, &n->zslots[i].buffer);
        }
        if (n->zread_program) glDeleteProgram(n->zread_program);
        glDeleteProgram(n->present_program);
        dhdr_tex_shutdown();
    }
    free(n);
}

void dhdr_native_options(DhdrNative *n, uint32_t flags)
{
    if (n) n->point_filter = (flags & DHDR_NATIVE_POINT_FILTER) != 0;
}

/* ---- surfaces ---- */

int dhdr_native_surface(DhdrNative *n, uint32_t index, uint32_t scale, uint32_t w, uint32_t h, uint32_t depth,
                        uint32_t size_out[2])
{
    if (!n || index >= DHDR_NATIVE_SURFACE_COUNT || !ensure_gl_objects(n)) { size_out[0] = size_out[1] = 0; return 0; }
    NativeSurface *s = &n->surfaces[index];
    if (s->texture && s->width >= w && s->height >= h) {
        size_out[0] = s->width; size_out[1] = s->height;
        return 1;
    }
    uint64_t began = SDL_GetTicksNS();
    g_cost.surfaces++;
    uint32_t nw = s->texture && s->width > w ? s->width : (w ? w : 1);
    uint32_t nh = s->texture && s->height > h ? s->height : (h ? h : 1);
    GLuint texture = 0, fbo = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, depth ? GL_DEPTH_COMPONENT32F : GL_RGBA8, (GLsizei)nw, (GLsizei)nh);
    /* Nearest and one level for a Z surface too: the Z probe fetches from it, and the default
       minifying filter wants mipmaps it does not have, which makes a fetch undefined. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, depth ? GL_DEPTH_ATTACHMENT : GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (!depth) { glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT); }
    /* zero, not one: dhd_render.h gives a new surface zero texels, and the GS tests GREATER
       against them, which a one would reject every draw against. */
    else { glDepthMask(GL_TRUE); glClearDepthf(0.0f); glClear(GL_DEPTH_BUFFER_BIT); }
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "gs native: surface %u's framebuffer is incomplete (%u x %u)\n", index, nw, nh);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteTextures(1, &texture);
        glDeleteFramebuffers(1, &fbo);
        size_out[0] = s->width; size_out[1] = s->height;
        return s->texture != 0;
    }
    if (s->texture) {   /* keep the old content (dhdr_native_surface grows, it never clears) */
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s->fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        glBlitFramebuffer(0, 0, (GLint)s->width, (GLint)s->height, 0, 0, (GLint)s->width, (GLint)s->height,
                          depth ? GL_DEPTH_BUFFER_BIT : GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glDeleteTextures(1, &s->texture);
        glDeleteFramebuffers(1, &s->fbo);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s->texture = texture; s->fbo = fbo; s->width = nw; s->height = nh; s->scale = scale ? scale : 1; s->depth = depth != 0;
    s->depth_attached = 0;   /* a new framebuffer carries none of the old one's attachments */
    size_out[0] = nw; size_out[1] = nh;
    g_cost.surface_ns += SDL_GetTicksNS() - began;
    return 1;
}

/* ---- scanout (the display surface into the display texture) and the movie frame ---- */

static void ensure_display_texture(DhdrNative *n, uint32_t w, uint32_t h)
{
    if (n->display_texture && n->display_width == w && n->display_height == h) return;
    if (n->display_fbo) { glDeleteFramebuffers(1, &n->display_fbo); n->display_fbo = 0; }
    if (n->display_texture) { glDeleteTextures(1, &n->display_texture); n->display_texture = 0; }
    n->display_width = n->display_height = 0;
    if (!w || !h) return;
    glGenTextures(1, &n->display_texture);
    glBindTexture(GL_TEXTURE_2D, n->display_texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, (GLsizei)w, (GLsizei)h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &n->display_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, n->display_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, n->display_texture, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    n->display_width = w; n->display_height = h;
}

/* native_scanout (scanout.rs's Display::scanout): the display surface's 12 display words, its own
   scale filled in at d[8], dispatched over the (upscaled) display size. False: no display surface,
   no texture on it yet, or a display too large. */
static int scan_out(DhdrNative *n, const DhdrPicture *p)
{
    if (p->display_surface < 0 || (uint32_t)p->display_surface >= DHDR_NATIVE_SURFACE_COUNT) return 0;
    NativeSurface *s = &n->surfaces[p->display_surface];
    if (!s->texture || s->depth) return 0;
    uint32_t d[12];
    memcpy(d, p->display, sizeof d);
    d[5] *= s->scale; d[6] *= s->scale; d[8] = s->scale;
    if (!d[5] || !d[6] || d[5] > 8192 || d[6] > 8192) return 0;
    ensure_display_texture(n, d[5], d[6]);
    if (!n->display_texture) return 0;
    int mode = n->scanout_mode;
    if (mode == SCAN_NONE) return 1;
    if (mode != SCAN_COMPUTE) {
        /* The draw forms: every state the draw loop may have left is set here, so the present
           pass after it starts from plain state too. The draw loop's own held state assumes
           the depth test and blending off, as left here. */
        glBindFramebuffer(GL_FRAMEBUFFER, n->display_fbo);
        glViewport(0, 0, (GLsizei)d[5], (GLsizei)d[6]);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDisable(GL_BLEND);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glUseProgram(n->scanout[mode].id);
        glUniform4ui(n->scanout[mode].da, d[0], d[1], d[2], d[3]);
        glUniform4ui(n->scanout[mode].db, d[4], d[5], d[6], d[7]);
        glUniform1ui(n->scanout[mode].ds, d[8]);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s->texture);
        glBindVertexArray(n->vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return 1;
    }
    glUseProgram(n->scanout[SCAN_COMPUTE].id);
    glUniform4ui(n->scanout[SCAN_COMPUTE].da, d[0], d[1], d[2], d[3]);
    glUniform4ui(n->scanout[SCAN_COMPUTE].db, d[4], d[5], d[6], d[7]);
    glUniform1ui(n->scanout[SCAN_COMPUTE].ds, d[8]);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s->texture);
    glBindImageTexture(1, n->display_texture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glDispatchCompute((d[5] + 7) / 8, (d[6] + 7) / 8, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    return 1;
}

/* native_movie_scanout (scanout.rs's Display::movie): the movie frame written straight into the
   display texture. */
static int scan_out_movie(DhdrNative *n, uint32_t w, uint32_t h, const uint32_t *pixels)
{
    if (!w || !h || !pixels) return 0;
    ensure_display_texture(n, w, h);
    if (!n->display_texture) return 0;
    glBindTexture(GL_TEXTURE_2D, n->display_texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    return 1;
}

/* ---- the texture copies: DHDR_CMD_COPY and IMPORT (surfaces.rs's transfer) ----

   The Y flip again: a surface texture holds the GS picture upside down, because native_vertex
   puts GS y = 0 at the last texture row. So a GS rectangle `y` high at `h` lives at texture row
   `surface height - y - h`, and every rectangle below is turned round on its way in or out.

   ALPHA_UPLOAD is still a no-op: native.wgsl's native_upload_alpha keeps the target's RGB and
   replaces bits of its alpha from a 64 x 32 upload whose red is the new bits and whose green is
   the mask, which needs a pass of its own over a copy of the previous content rather than the
   plain rectangle copies here. */

/* A GS rectangle's texture row (the surface's own pixels, not GS ones). */
static int32_t flip_y(uint32_t height, int32_t y, int32_t h) { return (int32_t)height - y - h; }

static int ensure_aux(GLuint *texture, GLuint *fbo, uint32_t *have_w, uint32_t *have_h,
                      uint32_t w, uint32_t h)
{
    if (*texture && *have_w >= w && *have_h >= h) return 1;
    uint32_t nw = *have_w > w ? *have_w : w, nh = *have_h > h ? *have_h : h;
    if (!nw || !nh || nw > 8192 || nh > 8192) return 0;
    if (*fbo) glDeleteFramebuffers(1, fbo);
    if (*texture) glDeleteTextures(1, texture);
    *texture = *fbo = 0; *have_w = *have_h = 0;
    glGenTextures(1, texture);
    glBindTexture(GL_TEXTURE_2D, *texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, (GLsizei)nw, (GLsizei)nh);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *texture, 0);
    int ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!ok) { glDeleteFramebuffers(1, fbo); glDeleteTextures(1, texture); *texture = *fbo = 0; return 0; }
    *have_w = nw; *have_h = nh;
    return 1;
}

/* native_copy: a w x h rectangle of GS pixels from surface `source` at (sx, sy) to `target` at
   (x, y). The scales matching, the texels move as they are; differing, the rectangle is blitted
   and resampled, as surfaces.rs does. */
static void copy_surface(DhdrNative *n, const DhdrCommand *k, DhdrPictureResult *out)
{
    if (k->source < 0 || (uint32_t)k->source >= DHDR_NATIVE_SURFACE_COUNT) return;
    if (k->target < 0 || (uint32_t)k->target >= DHDR_NATIVE_SURFACE_COUNT) return;
    NativeSurface *s = &n->surfaces[k->source], *t = &n->surfaces[k->target];
    if (!s->texture || !t->texture || s->depth || t->depth) return;
    uint32_t ss = s->scale, ts = t->scale;
    if (!ss || !ts) return;
    int32_t w = k->w, h = k->h, sx = k->sx, sy = k->sy, x = k->x, y = k->y;
    int32_t sw = (int32_t)(s->width / ss), sh = (int32_t)(s->height / ss);
    int32_t tw = (int32_t)(t->width / ts), th = (int32_t)(t->height / ts);
    if (sx + w > sw) w = sw - sx;
    if (sy + h > sh) h = sh - sy;
    if (x + w > tw) w = tw - x;
    if (y + h > th) h = th - y;
    if (w <= 0 || h <= 0 || sx < 0 || sy < 0 || x < 0 || y < 0) return;
    out->copies++;
    out->copy_px += (uint64_t)w * (uint64_t)h;
    if (ss != ts) {   /* resampled: the only way between textures of different sizes */
        int32_t s0 = (int32_t)(sx * (int32_t)ss), s1 = s0 + w * (int32_t)ss;
        int32_t d0 = (int32_t)(x * (int32_t)ts), d1 = d0 + w * (int32_t)ts;
        int32_t sy0 = flip_y(s->height, sy * (int32_t)ss, h * (int32_t)ss);
        int32_t dy0 = flip_y(t->height, y * (int32_t)ts, h * (int32_t)ts);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s->fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, t->fbo);
        glDisable(GL_SCISSOR_TEST);
        glBlitFramebuffer(s0, sy0, s1, sy0 + h * (int32_t)ss, d0, dy0, d1, dy0 + h * (int32_t)ts,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glEnable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }
    GLint bx = sx * (int32_t)ss, bw = w * (int32_t)ss, bh = h * (int32_t)ss;
    GLint dx = x * (int32_t)ss;
    GLint by = flip_y(s->height, sy * (int32_t)ss, bh), dy = flip_y(t->height, y * (int32_t)ss, bh);
    if (k->source == k->target) {
        /* One texture read and written at once is undefined, so it goes out and back. */
        if (!ensure_aux(&n->scratch_texture, &n->scratch_fbo, &n->scratch_width, &n->scratch_height,
                        (uint32_t)bw, (uint32_t)bh))
            return;
        glCopyImageSubData(s->texture, GL_TEXTURE_2D, 0, bx, by, 0,
                           n->scratch_texture, GL_TEXTURE_2D, 0, 0, 0, 0, bw, bh, 1);
        glCopyImageSubData(n->scratch_texture, GL_TEXTURE_2D, 0, 0, 0, 0,
                           t->texture, GL_TEXTURE_2D, 0, dx, dy, 0, bw, bh, 1);
        out->self_copies++;
        out->self_copy_px += (uint64_t)w * (uint64_t)h;
        return;
    }
    glCopyImageSubData(s->texture, GL_TEXTURE_2D, 0, bx, by, 0,
                       t->texture, GL_TEXTURE_2D, 0, dx, dy, 0, bw, bh, 1);
}

/* native_import: w x h RGBA8 pixels (rows from the top, as the GS has them) into the target
   surface at (x, y), scaled up to the surface's own scale. */
static void import_pixels(DhdrNative *n, const DhdrCommand *k, DhdrPictureResult *out)
{
    if (k->target < 0 || (uint32_t)k->target >= DHDR_NATIVE_SURFACE_COUNT || !k->pixels) return;
    NativeSurface *t = &n->surfaces[k->target];
    if (!t->texture || t->depth) return;
    uint32_t ts = t->scale ? t->scale : 1;
    int32_t w = k->w, h = k->h, x = k->x, y = k->y;
    int32_t tw = (int32_t)(t->width / ts), th = (int32_t)(t->height / ts);
    if (w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > tw || y + h > th) return;
    if (!ensure_aux(&n->import_texture, &n->import_fbo, &n->import_width, &n->import_height,
                    (uint32_t)w, (uint32_t)h))
        return;
    glBindTexture(GL_TEXTURE_2D, n->import_texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, k->pixels);
    /* The destination rectangle is given upside down, which is what turns the rows round: the
       pixels arrive with row 0 at the GS top, and the surface keeps the GS top last. */
    int32_t d0 = x * (int32_t)ts, d1 = d0 + w * (int32_t)ts;
    int32_t low = flip_y(t->height, y * (int32_t)ts, h * (int32_t)ts);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, n->import_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, t->fbo);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, w, h, d0, low + h * (int32_t)ts, d1, low, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    out->imports++;
    out->import_px += (uint64_t)w * (uint64_t)h;
}

/* ---- the picture: native_render's command loop ---- */

static int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

static void ensure_buffer(GLenum target, GLuint buffer, size_t *capacity, size_t bytes)
{
    glBindBuffer(target, buffer);
    if (bytes > *capacity) {
        size_t grown = bytes + bytes / 2;
        glBufferData(target, (GLsizeiptr)grown, NULL, GL_DYNAMIC_DRAW);
        *capacity = grown;
    }
}

/* ---- depth and blending (pipelines.rs's key, as GL state) ---- */

/* A draw's depth state: pipelines.rs's 5-bit depth field read back out. `on` is its attachment
   bit; the GS function numbers are 0 never, 1 always, 2 gequal, 3 greater, 4 equal (5-7 always),
   which is what depth_compare maps. */
typedef struct DepthState { int on, write; uint32_t function; } DepthState;
typedef struct BlendState { int on; GLenum src, dst, op; } BlendState;

static GLenum depth_func(uint32_t function)
{
    switch (function) {
    case 0: return GL_NEVER;
    case 2: return GL_GEQUAL;
    case 3: return GL_GREATER;
    case 4: return GL_EQUAL;
    default: return GL_ALWAYS;
    }
}

/* D3D11's blend factor numbers, the ones the GS blend uses (pipelines.rs's blend_factor); every
   other number is One there too. Without dual-source blending SRC_ALPHA and INV_SRC_ALPHA are the
   plain source alpha, which is why a draw blending on As is split into two. */
static GLenum blend_factor(uint32_t d3d)
{
    switch (d3d) {
    case 1: return GL_ZERO;
    case 2: return GL_ONE;
    case 5: return GL_SRC_ALPHA;
    case 6: return GL_ONE_MINUS_SRC_ALPHA;
    case 7: return GL_DST_ALPHA;
    case 8: return GL_ONE_MINUS_DST_ALPHA;
    case 14: return GL_CONSTANT_COLOR;
    case 15: return GL_ONE_MINUS_CONSTANT_COLOR;
    default: return GL_ONE;
    }
}

/* D3D11's operation numbers (pipelines.rs's blend_operation); 6 and 7 have no pipeline there, and
   0 is wgpu-native's default, Add. 0 back: the draw is skipped, as the Rust renderer skips it. */
static GLenum blend_op(uint32_t d3d)
{
    switch (d3d) {
    case 0: case 1: return GL_FUNC_ADD;
    case 2: return GL_FUNC_SUBTRACT;
    case 3: return GL_FUNC_REVERSE_SUBTRACT;
    case 4: return GL_MIN;
    case 5: return GL_MAX;
    default: return 0;
    }
}

enum { BLEND_SA = 5, BLEND_ISA = 6 };   /* D3D11_BLEND_SRC_ALPHA, _INV_SRC_ALPHA (render.rs) */

static void set_depth(DepthState *held, DepthState want)
{
    if (held->on != want.on) {
        if (want.on) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        held->on = want.on;
        held->function = 0xFFFFFFFFu;   /* the func is only meaningful while the test is on */
    }
    if (want.on && held->function != want.function) {
        glDepthFunc(depth_func(want.function));
        held->function = want.function;
    }
    if (held->write != want.write) {
        glDepthMask(want.write ? GL_TRUE : GL_FALSE);
        held->write = want.write;
    }
}

static void set_mask(uint32_t *held, uint32_t mask)
{
    if (*held == mask) return;
    glColorMask((mask & 1) != 0, (mask & 2) != 0, (mask & 4) != 0, (mask & 8) != 0);
    *held = mask;
}

/* 255 is native_pixel's divisor, 128 native_pixel_blend's. A DrawProgram is zeroed when it is
   made, and neither value is zero, so the first call always sends it. */
static void set_alpha_div(DrawProgram *d, float div)
{
    if (d->h_alpha_div == div) return;
    glUniform1f(d->u_alpha_div, div);
    d->h_alpha_div = div;
}

static void issue(const uint32_t *indices, uint32_t index_count, uint32_t first, uint32_t count)
{
    if (indices && index_count)
        glDrawElements(GL_TRIANGLES, (GLsizei)count, GL_UNSIGNED_INT,
                       (const void *)(uintptr_t)((size_t)first * sizeof(uint32_t)));
    else
        glDrawArrays(GL_TRIANGLES, (GLint)first, (GLsizei)count);
}

static void set_blend(BlendState *held, BlendState want)
{
    if (held->on != want.on) {
        if (want.on) glEnable(GL_BLEND); else glDisable(GL_BLEND);
        held->on = want.on;
    }
    if (!want.on) return;
    if (held->src != want.src || held->dst != want.dst) {
        /* The colour blend is the draw's; the alpha channel is written as it comes (One, Zero,
           Add), as pipelines.rs builds every BlendState. */
        glBlendFuncSeparate(want.src, want.dst, GL_ONE, GL_ZERO);
        held->src = want.src; held->dst = want.dst;
    }
    if (held->op != want.op) {
        glBlendEquationSeparate(want.op, GL_FUNC_ADD);
        held->op = want.op;
    }
}

/* DHD_GL_TIMES: where the render thread's time goes, which the perf lines cannot say (they see
   one `render` thread). 1 times the CPU side of each phase, 2 puts a glFinish between the phases
   so the GPU's own time lands in the phase that caused it (much slower; for one measurement, not
   for play). A line every 300 pictures.

   The phases are the picture's three parts: the draw loop (every DHDR_CMD_DRAW, with the texture
   uploads a first sample pays for), the scanout compute shader and, separately, what
   textures.c spent uploading. */
static int gl_times(void)
{
    static int level = -1;
    if (level < 0) {
        /* getenv, not SDL_getenv: dhd.ini reaches the process environment through
           host_putenv_s after SDL took its own copy, so SDL_getenv would never see it. */
        const char *s = getenv("DHD_GL_TIMES");
        level = s ? atoi(s) : 0;
    }
    return level;
}

static uint64_t phase_end(int level)
{
    if (level >= 2) glFinish();
    return SDL_GetTicksNS();
}

/* 1 when it printed a line (the window it summed is over). */
static int report_times(uint64_t draws_ns, uint64_t scan_ns, uint64_t collect_ns, uint64_t draws,
                        uint64_t programs, uint64_t textures, uint64_t fconvert, uint64_t fsurface,
                        uint64_t fother, const char *scanout)
{
    static uint64_t pictures, a_draws_ns, a_scan_ns, a_collect_ns, a_draws, a_programs, a_textures;
    static uint64_t a_fconvert, a_fsurface, a_fother;
    static uint64_t was_uploads, was_upload_ns, was_bytes, since;
    a_draws_ns += draws_ns; a_scan_ns += scan_ns; a_collect_ns += collect_ns;
    a_draws += draws; a_programs += programs; a_textures += textures;
    a_fconvert += fconvert; a_fsurface += fsurface; a_fother += fother;
    uint64_t now = SDL_GetTicksNS();
    if (!since) since = now;
    /* 300 pictures or five seconds, whichever comes first: at 60 fields a second that is a line
       every five seconds, and at three (the rate this was written to explain) it still is, rather
       than one line every hundred seconds. */
    if (++pictures < 300 && now - since < 5000000000ull) return 0;
    since = now;
    uint64_t uploads, upload_ns, bytes;
    dhdr_tex_upload_stats(&uploads, &upload_ns, &bytes);
    double n = (double)pictures;
    fprintf(stderr, "gs native: per picture over %u: draws %.2f ms (%.0f draws, %.0f program changes, "
                    "%.0f texture binds), scanout (%s) %.2f ms, collect %.2f ms | uploads %.1f (%.2f ms, "
                    "%.0f KiB) | flat %.1f convert, %.1f surface, %.1f other\n",
            (unsigned)pictures, a_draws_ns / n / 1e6, a_draws / n, a_programs / n, a_textures / n,
            scanout, a_scan_ns / n / 1e6, a_collect_ns / n / 1e6, (uploads - was_uploads) / n,
            (upload_ns - was_upload_ns) / n / 1e6, (bytes - was_bytes) / n / 1024.0,
            a_fconvert / n, a_fsurface / n, a_fother / n);
    was_uploads = uploads; was_upload_ns = upload_ns; was_bytes = bytes;
    pictures = a_draws_ns = a_scan_ns = a_collect_ns = a_draws = a_programs = a_textures = 0;
    a_fconvert = a_fsurface = a_fother = 0;
    return 1;
}

/* A draw sampling its own target t (surfaces.rs's sample_target): the region its texture can
   reach (tex_offset, tex_size, cut to the surface) copied to the scratch texture at the same
   texture coordinates, which the draw then samples with the surface's own size and rows. The
   scratch texture, or 0 to draw untextured (the caller's flat fallback). */
static GLuint sample_target(DhdrNative *n, NativeSurface *t, const DhdrNativeConstants *c, DhdrPictureResult *out)
{
    uint32_t ss = t->scale ? t->scale : 1;
    int32_t x = c->tex_offset[0], y = c->tex_offset[1];
    int32_t w = (int32_t)c->tex_size[0], h = (int32_t)c->tex_size[1];
    int32_t sw = (int32_t)(t->width / ss), sh = (int32_t)(t->height / ss);
    if (x < 0 || y < 0) return 0;
    if (x + w > sw) w = sw - x;
    if (y + h > sh) h = sh - y;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    if (!ensure_aux(&n->scratch_texture, &n->scratch_fbo, &n->scratch_width, &n->scratch_height, t->width, t->height))
        return 0;
    if (w && h) {
        GLint bx = x * (int32_t)ss, bw = w * (int32_t)ss, bh = h * (int32_t)ss;
        GLint by = flip_y(t->height, y * (int32_t)ss, bh);
        glCopyImageSubData(t->texture, GL_TEXTURE_2D, 0, bx, by, 0,
                           n->scratch_texture, GL_TEXTURE_2D, 0, bx, by, 0, bw, bh, 1);
    }
    out->self_copies++;
    out->self_copy_px += (uint64_t)w * (uint64_t)h;
    return n->scratch_texture;
}

/* native_upload_alpha (native.wgsl): a 64 x 32 page of alpha bits applied to a surface without
   resampling its scaled RGB. The upload's red is the new bits and its green the mask: the alpha
   becomes (old & ~mask) | bits, read from a copy of the region in the scratch texture. Drawn as
   one triangle over the target, cut to the page by the scissor. */
static const char *upload_alpha_source =
    "uniform usampler2D upload;\n"     /* 64 x 32 RGBA8UI, row 0 the GS top */
    "uniform sampler2D previous;\n"    /* the target's copy, the same texel layout */
    "uniform ivec4 map;\n"             /* x, y (GS pixels), scale, the target's height */
    "out vec4 color;\n"
    "void main() {\n"
    "    ivec2 texel = ivec2(gl_FragCoord.xy);\n"
    "    ivec2 gs = ivec2(texel.x, map.w - 1 - texel.y) / map.z - map.xy;\n"
    "    uvec2 bits = (all(greaterThanEqual(gs, ivec2(0))) && all(lessThan(gs, ivec2(64, 32))))\n"
    "               ? texelFetch(upload, gs, 0).rg : uvec2(0u);\n"
    "    vec4 old = texelFetch(previous, texel, 0);\n"
    "    uint alpha = uint(round(old.a * 255.0));\n"
    "    color = vec4(old.rgb, float((alpha & ~bits.y) | bits.x) / 255.0);\n"
    "}\n";

/* The alpha upload's program and its 64 x 32 texture, made once (at start-up, prelink_programs). */
static int ensure_upload(DhdrNative *n)
{
    static int failed;
    if (n->upload_program) return 1;
    if (failed) return 0;
    n->upload_program = link_program(scanout_vertex_source, "#version 430 core\n", upload_alpha_source, NULL);
    if (!n->upload_program) { failed = 1; return 0; }
    glUseProgram(n->upload_program);
    glUniform1i(glGetUniformLocation(n->upload_program, "upload"), 1);
    glUniform1i(glGetUniformLocation(n->upload_program, "previous"), 0);
    n->upload_map = glGetUniformLocation(n->upload_program, "map");
    glGenTextures(1, &n->upload_texture);
    glBindTexture(GL_TEXTURE_2D, n->upload_texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8UI, 64, 32);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return 1;
}

static void alpha_upload(DhdrNative *n, const DhdrCommand *k)
{
    if (k->target < 0 || (uint32_t)k->target >= DHDR_NATIVE_SURFACE_COUNT || !k->pixels) return;
    NativeSurface *t = &n->surfaces[k->target];
    if (!t->texture || t->depth) return;
    int32_t ss = (int32_t)(t->scale ? t->scale : 1);
    int32_t left = k->x * ss, top = k->y * ss, right = (k->x + 64) * ss, bottom = (k->y + 32) * ss;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > (int32_t)t->width) right = (int32_t)t->width;
    if (bottom > (int32_t)t->height) bottom = (int32_t)t->height;
    if (right <= left || bottom <= top) return;
    if (!ensure_upload(n)) return;
    if (!ensure_aux(&n->scratch_texture, &n->scratch_fbo, &n->scratch_width, &n->scratch_height, t->width, t->height))
        return;
    GLint low = (GLint)t->height - bottom;   /* the page's lowest texture row */
    glCopyImageSubData(t->texture, GL_TEXTURE_2D, 0, left, low, 0,
                       n->scratch_texture, GL_TEXTURE_2D, 0, left, low, 0, right - left, bottom - top, 1);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, n->upload_texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 64, 32, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, k->pixels);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, n->scratch_texture);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glViewport(0, 0, (GLsizei)t->width, (GLsizei)t->height);
    glEnable(GL_SCISSOR_TEST);
    glScissor(left, low, right - left, bottom - top);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(n->upload_program);
    glUniform4i(n->upload_map, k->x, k->y, ss, (GLint)t->height);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

/* ---- conversions (native.wgsl's native_convert, convert.rs) ----

   A texture over GS memory some of which native surfaces drew: a compute pass rebuilds it texel
   by texel, each GS word taken from the decoded base texture (page source 0) or from one of up to
   four source surfaces (colour or Z), through the swizzle tables, then the texture's own format
   and palette applied. Translated line for line from native.wgsl; what differs:
   - the Y flip: a surface holds its GS rows bottom up (native_vertex), so a source is read at
     texture row (height - 1 - y), and the converted texture is written the same way round, so a
     draw samples it exactly as it samples a surface (fetch_surface, texels_size its size);
   - the page arithmetic divides by powers of two with shifts (pw, ph are 64 or 128, 32 or 64):
     this GPU has no integer divide (STATE.md NX3); the divisions by the buffer width stay.
   - a Z source is a sampler2D over the depth texture (compare mode off), reading its .r. */
static const char *convert_source =
    "#version 430 core\n"
    "layout(local_size_x = 8, local_size_y = 8) in;\n"
    "layout(std140, binding = 0) uniform Convert {\n"
    "    uvec4 tex; uvec4 size; uvec4 slot[4]; uvec4 pages[32]; uvec4 palette[64];\n"
    "} convert;\n"
    "layout(std430, binding = 0) readonly buffer Inverse { uint native_inverse[]; };\n"
    "layout(binding = 0) uniform usampler2D convert_base;\n"
    "layout(binding = 1) uniform sampler2D convert_colour0;\n"
    "layout(binding = 2) uniform sampler2D convert_colour1;\n"
    "layout(binding = 3) uniform sampler2D convert_colour2;\n"
    "layout(binding = 4) uniform sampler2D convert_colour3;\n"
    "layout(binding = 5) uniform sampler2D convert_depth0;\n"
    "layout(binding = 6) uniform sampler2D convert_depth1;\n"
    "layout(binding = 7) uniform sampler2D convert_depth2;\n"
    "layout(binding = 8) uniform sampler2D convert_depth3;\n"
    "layout(rgba8, binding = 0) writeonly uniform image2D native_converted;\n"
    /* A surface's texel at GS-scaled (x, y), rows from the top; 0 outside. */
    "vec4 load_surface(sampler2D t, ivec2 xy) {\n"
    "    ivec2 size = textureSize(t, 0);\n"
    "    if (any(lessThan(xy, ivec2(0))) || any(greaterThanEqual(xy, size))) return vec4(0.0);\n"
    "    return texelFetch(t, ivec2(xy.x, size.y - 1 - xy.y), 0);\n"
    "}\n"
    "uint load_base(ivec2 xy) {\n"
    "    ivec2 size = textureSize(convert_base, 0);\n"
    "    if (any(lessThan(xy, ivec2(0))) || any(greaterThanEqual(xy, size))) return 0u;\n"
    "    return texelFetch(convert_base, xy, 0).x;\n"
    "}\n"
    "uint bits_per_pixel(uint psm) {\n"
    "    if (psm == 1u || psm == 49u) return 24u;\n"
    "    if ((psm & 7u) == 2u) return 16u;\n"
    "    if (psm == 19u || psm == 27u) return 8u;\n"
    "    if (psm == 20u || psm == 36u || psm == 44u) return 4u;\n"
    "    return 32u;\n"
    "}\n"
    "uint address(uint psm, uint bp, uint bw, uint x, uint y) {\n"
    "    uint pws = 6u, phs = 5u, bits = 32u, high = 0u, block = 0u, column = 0u, bx = 0u, by = 0u;\n"
    "    if (psm == 19u || psm == 20u) { pws = 7u; phs = (psm == 19u) ? 6u : 7u; }\n"
    "    else if ((psm & 7u) == 2u) { phs = 6u; }\n"
    "    uint px = x & ((1u << pws) - 1u);\n"
    "    uint py = y & ((1u << phs) - 1u);\n"
    "    if (psm == 19u) {\n"
    "        bits = 8u; bx = px >> 4; by = py >> 4;\n"
    "        block = (bx & 1u) | (by & 1u) << 1 | (bx & 2u) << 1 | (by & 2u) << 2 | (bx & 4u) << 2;\n"
    "        column = (px & 1u) << 2 | (px & 2u) << 3 | (px & 4u) << 3 | (px & 8u) >> 2 |\n"
    "                 (py & 1u) << 3 | (py & 2u) >> 1 | (py & 4u) << 4 | (py & 8u) << 4;\n"
    "        column ^= ((py >> 1 ^ py >> 2) & 1u) << 5;\n"
    "    } else if (psm == 20u) {\n"
    "        bits = 4u; bx = px >> 5; by = py >> 4;\n"
    "        block = (by & 1u) | (bx & 1u) << 1 | (by & 2u) << 1 | (bx & 2u) << 2 | (by & 4u) << 2;\n"
    "        column = (px & 1u) << 3 | (px & 2u) << 4 | (px & 4u) << 4 | (px & 8u) >> 2 | (px & 16u) >> 2 |\n"
    "                 (py & 1u) << 4 | (py & 2u) >> 1 | (py & 4u) << 5 | (py & 8u) << 5;\n"
    "        column ^= ((py >> 1 ^ py >> 2) & 1u) << 6;\n"
    "    } else if ((psm & 7u) == 2u) {\n"
    "        bits = 16u; bx = px >> 4; by = py >> 3;\n"
    "        if ((psm & 8u) != 0u) {\n"
    "            block = (by & 1u) | (bx & 1u) << 1 | (by & 4u) | (by & 2u) << 2 | (bx & 2u) << 3;\n"
    "        } else {\n"
    "            block = (by & 1u) | (bx & 1u) << 1 | (by & 2u) << 1 | (bx & 2u) << 2 | (by & 4u) << 2;\n"
    "        }\n"
    "        column = (px & 1u) << 1 | (px & 2u) << 2 | (px & 4u) << 2 | (px & 8u) >> 3 |\n"
    "                 (py & 1u) << 2 | (py & 6u) << 4;\n"
    "    } else {\n"
    "        bx = px >> 3; by = py >> 3;\n"
    "        block = (bx & 1u) | (by & 1u) << 1 | (bx & 2u) << 1 | (by & 2u) << 2 | (bx & 4u) << 2;\n"
    "        column = (px & 1u) | (px & 6u) << 1 | (py & 1u) << 1 | (py & 6u) << 3;\n"
    "        if (psm == 27u || psm == 36u) high = 24u;\n"
    "        if (psm == 44u) high = 28u;\n"
    "    }\n"
    "    block += bp + ((y >> phs) * ((bw << 6) >> pws) + (x >> pws)) * 32u;\n"
    "    if ((psm & 48u) == 48u) block ^= 24u;\n"
    "    return (block * 2048u + column * bits + high) & 0x1FFFFFFu;\n"
    "}\n"
    "uint convert_word(uint slot, ivec2 xy, bool depth, float unit) {\n"
    "    if (depth) {\n"
    "        float d;\n"
    "        if (slot == 0u) d = load_surface(convert_depth0, xy).r; else if (slot == 1u) d = load_surface(convert_depth1, xy).r;\n"
    "        else if (slot == 2u) d = load_surface(convert_depth2, xy).r; else d = load_surface(convert_depth3, xy).r;\n"
    "        return (d >= 1.0) ? 0xFFFFFFFFu : uint(d * unit);\n"
    "    }\n"
    "    vec4 c;\n"
    "    if (slot == 0u) c = load_surface(convert_colour0, xy); else if (slot == 1u) c = load_surface(convert_colour1, xy);\n"
    "    else if (slot == 2u) c = load_surface(convert_colour2, xy); else c = load_surface(convert_colour3, xy);\n"
    "    uvec4 u = uvec4(round(c * 255.0));\n"
    "    return u.r | u.g << 8 | u.b << 16 | u.a << 24;\n"
    "}\n"
    "uint convert_half(uint slot, uvec2 origin, uint at, uint scale) {\n"
    "    uvec2 p = (origin + uvec2(at & 255u, at >> 8)) * scale + scale / 2u;\n"
    "    uint w = convert_word(slot, ivec2(p), false, 0.0);\n"
    "    return (w >> 3 & 31u) | (w >> 11 & 31u) << 5 | (w >> 19 & 31u) << 10 | (w >> 31) << 15;\n"
    "}\n"
    "void store(uvec2 at, uint rgba) {\n"
    "    imageStore(native_converted, ivec2(int(at.x), int(convert.size.y) - 1 - int(at.y)),\n"
    "               vec4(float(rgba & 255u), float(rgba >> 8 & 255u), float(rgba >> 16 & 255u), float(rgba >> 24)) / 255.0);\n"
    "}\n"
    "void main() {\n"
    "    uvec3 tid = gl_GlobalInvocationID;\n"
    "    if (tid.x >= convert.size.x || tid.y >= convert.size.y) return;\n"
    "    uint psm = convert.tex.x;\n"
    "    uint a = address(psm, convert.tex.y, convert.tex.z, tid.x, tid.y);\n"
    "    uint word = a >> 5;\n"
    "    uint page = (word >> 11) & 511u;\n"
    "    uint source = (convert.pages[page >> 4][(page >> 2) & 3u] >> ((page & 3u) * 8u)) & 255u;\n"
    "    if (source == 0u) { store(tid.xy, load_base(ivec2(tid.xy))); return; }\n"
    "    uvec4 s = convert.slot[source - 1u];\n"
    "    uint rel = (page - s.x) & 511u;\n"
    "    uint bw = s.y;\n"
    "    uint scale = convert.tex.w >> ((source - 1u) * 8u) & 255u;\n"
    "    uint value;\n"
    "    if (s.z == 2u) {\n"
    "        uvec2 origin = uvec2((rel % bw) * 64u, (rel / bw) * 64u);\n"
    "        uint half_at = 4096u + (word & 2047u) * 2u;\n"
    "        value = convert_half(source - 1u, origin, native_inverse[half_at], scale) |\n"
    "                convert_half(source - 1u, origin, native_inverse[half_at + 1u], scale) << 16;\n"
    "    } else {\n"
    "        uint at = native_inverse[((s.z == 1u) ? 2048u : 0u) + (word & 2047u)];\n"
    "        ivec2 xy = ivec2(int(((rel % bw) * 64u + (at & 255u)) * scale + scale / 2u),\n"
    "                         int(((rel / bw) * 32u + (at >> 8)) * scale + scale / 2u));\n"
    "        value = convert_word(source - 1u, xy, s.z == 1u, uintBitsToFloat(s.w));\n"
    "    }\n"
    "    uint bits = bits_per_pixel(psm);\n"
    "    uint texa = convert.size.z;\n"
    "    value = (value >> (a & 31u)) & ((bits == 32u) ? 0xFFFFFFFFu : ((1u << bits) - 1u));\n"
    "    uint rgba = value;\n"
    "    if (bits <= 8u) rgba = convert.palette[value >> 2][value & 3u];\n"
    "    else if ((psm & 7u) == 2u) {\n"
    "        uint alpha = ((value & 0x8000u) != 0u) ? (convert.size.w & 255u) : (texa & 255u);\n"
    "        if ((texa & 0x8000u) != 0u && value == 0u) alpha = 0u;\n"
    "        rgba = (value & 31u) << 3 | (value & 0x3E0u) << 6 | (value & 0x7C00u) << 9 | alpha << 24;\n"
    "    } else if ((psm & 7u) == 1u) {\n"
    "        rgba = (value & 0xFFFFFFu) | (((texa & 0x8000u) != 0u && (value & 0xFFFFFFu) == 0u) ? 0u : (texa & 255u) << 24);\n"
    "    }\n"
    "    store(tid.xy, rgba);\n"
    "}\n";

/* init.rs's inverse_table (native_core.inc's native_inverse_table): for the colour (PSMCT32)
   and Z (PSMZ32) layouts, the word in a page to the pixel holding it (x | y << 8), colour then
   Z; then for PSMCT16 (64 x 64 pages), the 16-bit half in a page to its pixel. */
static void inverse_table(uint32_t inverse[8192])
{
    memset(inverse, 0, 8192 * sizeof *inverse);
    for (uint32_t y = 0; y < 64; y++)
        for (uint32_t x = 0; x < 64; x++) {
            uint32_t bx = x / 16, by = y / 8;
            uint32_t block = (by & 1) | (bx & 1) << 1 | (by & 2) << 1 | (bx & 2) << 2 | (by & 4) << 2;
            uint32_t column = (x & 1) << 1 | (x & 2) << 2 | (x & 4) << 2 | (x & 8) >> 3 | (y & 1) << 2 | (y & 6) << 4;
            inverse[4096 + block * 128 + column] = x | y << 8;
        }
    for (uint32_t layout = 0; layout < 2; layout++)
        for (uint32_t y = 0; y < 32; y++)
            for (uint32_t x = 0; x < 64; x++) {
                uint32_t bx = x / 8, by = y / 8;
                uint32_t block = (bx & 1) | (by & 1) << 1 | (bx & 2) << 1 | (by & 2) << 2 | (bx & 4) << 2;
                uint32_t column = (x & 1) | (x & 6) << 1 | (y & 1) << 1 | (y & 6) << 3;
                if (layout == 1) block ^= 0x18;   /* PSMZ32: Z's block XOR */
                inverse[layout * 2048 + ((block * 2048 + column * 32) >> 5 & 2047)] = x | y << 8;
            }
}

/* The conversion program and its two buffers, made once. 0 when they could not be. */
static int ensure_convert(DhdrNative *n)
{
    static int failed;
    if (n->convert_program) return 1;
    if (failed) return 0;
    n->convert_program = link_program(NULL, NULL, NULL, convert_source);
    if (!n->convert_program) { failed = 1; return 0; }
    static uint32_t inverse[8192];
    inverse_table(inverse);
    glGenBuffers(1, &n->convert_inverse);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, n->convert_inverse);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof inverse, inverse, GL_STATIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    glGenBuffers(1, &n->convert_constants);
    glBindBuffer(GL_UNIFORM_BUFFER, n->convert_constants);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(DhdrConvertConstants), NULL, GL_STREAM_DRAW);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    return 1;
}

/* native_run_convert (convert.rs's run): conversion i of the picture into its converted texture.
   The texture (its size in w, h), or 0 when it cannot run: no size, a source surface without a
   texture, or no program. Leaves texture unit 0 active. */
static GLuint run_convert(DhdrNative *n, const DhdrPicture *p, int32_t i, uint32_t *w_out, uint32_t *h_out)
{
    if (i < 0 || (uint32_t)i >= p->convert_count || i >= CONVERTS || !p->converts) return 0;
    const DhdrConvert *v = &p->converts[i];
    uint32_t w = v->constants.size[0], h = v->constants.size[1];
    if (!w || !h || w > 4096 || h > 4096 || !ensure_convert(n)) return 0;
    DhdrConvertConstants constants = v->constants;
    constants.tex[3] = 0;   /* each source's scale, a byte a slot */
    GLuint colour[4] = { 0 }, depth[4] = { 0 };
    for (int s = 0; s < 4; s++) {
        uint32_t scale = 1;
        int16_t index = v->surface[s];
        if (index >= 0) {
            if ((uint32_t)index >= DHDR_NATIVE_SURFACE_COUNT) return 0;
            NativeSurface *src = &n->surfaces[index];
            if (!src->texture) return 0;
            if (src->depth) depth[s] = src->texture; else colour[s] = src->texture;
            scale = src->scale ? src->scale : 1;
        }
        constants.tex[3] |= scale << (s * 8);
    }
    if (n->converted[i].texture && (n->converted[i].w != w || n->converted[i].h != h)) {
        glDeleteTextures(1, &n->converted[i].texture);
        n->converted[i].texture = 0;
    }
    if (!n->converted[i].texture) {
        glGenTextures(1, &n->converted[i].texture);
        glBindTexture(GL_TEXTURE_2D, n->converted[i].texture);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, (GLsizei)w, (GLsizei)h);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        n->converted[i].w = w; n->converted[i].h = h;
    }
    GLuint base = v->base ? dhdr_tex_gl(v->base) : 0;
    glBindBuffer(GL_UNIFORM_BUFFER, n->convert_constants);
    glBufferData(GL_UNIFORM_BUFFER, sizeof constants, &constants, GL_STREAM_DRAW);   /* orphaned */
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, n->convert_constants);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, n->convert_inverse);
    glUseProgram(n->convert_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, base ? base : dhdr_tex_dummy_texels());
    for (int s = 0; s < 4; s++) {
        glActiveTexture(GL_TEXTURE1 + s);
        glBindTexture(GL_TEXTURE_2D, colour[s] ? colour[s] : dhdr_tex_dummy_rgba());
        glActiveTexture(GL_TEXTURE5 + s);
        glBindTexture(GL_TEXTURE_2D, depth[s] ? depth[s] : dhdr_tex_dummy_rgba());
    }
    glActiveTexture(GL_TEXTURE0);
    glBindImageTexture(0, n->converted[i].texture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glDispatchCompute((w + 7) / 8, (h + 7) / 8, 1);
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
    *w_out = w; *h_out = h;
    return n->converted[i].texture;
}

/* The Z surface a draw tests against (depth_key bit 3 says it has one, native_core.inc), or NULL. */
static NativeSurface *draw_depth(DhdrNative *n, const DhdrCommand *k)
{
    if (k->depth < 0 || (uint32_t)k->depth >= DHDR_NATIVE_SURFACE_COUNT || !(k->depth_key & 8)) return NULL;
    NativeSurface *d = &n->surfaces[k->depth];
    return d->texture && d->depth ? d : NULL;
}

/* A blend on As (SA or ISA on either side): without dual-source blending, two draws. */
static int blend_uses_as(uint32_t blend_key)
{
    uint32_t src = blend_key >> 1 & 31, dst = blend_key >> 6 & 31;
    return (blend_key & 1) && (src == BLEND_SA || src == BLEND_ISA || dst == BLEND_SA || dst == BLEND_ISA);
}

/* The reversed triangles (render.rs's draw_passes, arenas.rs's reverse_triangles): the alpha-only
   second draw of a GREATER-with-writes draw tests EQUAL against the Z its first draw left. Where
   two of its triangles overlap at the same Z, the first draw kept the earlier one (a later one is
   not GREATER), but EQUAL lets both through and the later one's alpha would win. Drawn in reverse
   order, the earlier triangle is drawn last and its alpha is what stays, as in the first draw.

   The reversed copies go after the picture's own vertices (or indices, for an indexed picture) in
   the same buffer; `reversed_first[i]` is where command i's copy starts, or UINT32_MAX for none. */
typedef struct Reversed {
    uint32_t *first;       size_t first_cap;
    DhdrVertex *vertices;  size_t vertex_cap, vertex_count;
    uint32_t *indices;     size_t index_cap, index_count;
    int valid;             /* `first` holds this picture's commands (0: out of memory, none reversed) */
} Reversed;

static int grow(void **buffer, size_t *cap, size_t want, size_t size)
{
    if (want <= *cap) return 1;
    size_t grown = want + want / 2 + 64;
    void *b = realloc(*buffer, grown * size);
    if (!b) return 0;
    *buffer = b; *cap = grown;
    return 1;
}

static void reverse_draws(DhdrNative *n, const DhdrPicture *p, const uint32_t *indices, uint32_t index_count,
                          Reversed *r)
{
    r->vertex_count = r->index_count = 0;
    r->valid = grow((void **)&r->first, &r->first_cap, p->command_count ? p->command_count : 1, sizeof *r->first);
    if (!r->valid) return;
    uint32_t base = indices && index_count ? index_count : p->vertex_count;
    for (uint32_t i = 0; i < p->command_count; i++) {
        const DhdrCommand *k = &p->commands[i];
        r->first[i] = UINT32_MAX;
        if (k->kind != DHDR_CMD_DRAW || k->count <= 3 || k->count % 3 || !(k->color_mask & 8)) continue;
        if ((k->depth_key & 7) != (4 | 3) || !blend_uses_as(k->blend_key)) continue;   /* GREATER, writes */
        if ((uint64_t)k->first + k->count > base || !draw_depth(n, k)) continue;
        if (indices && index_count) {
            if (!grow((void **)&r->indices, &r->index_cap, r->index_count + k->count, sizeof *r->indices)) continue;
            r->first[i] = base + (uint32_t)r->index_count;
            for (uint32_t t = 0; t < k->count; t += 3)
                memcpy(&r->indices[r->index_count + t], &indices[k->first + k->count - 3 - t], 3 * sizeof *indices);
            r->index_count += k->count;
        } else {
            if (!grow((void **)&r->vertices, &r->vertex_cap, r->vertex_count + k->count, sizeof *r->vertices)) continue;
            r->first[i] = base + (uint32_t)r->vertex_count;
            for (uint32_t t = 0; t < k->count; t += 3)
                memcpy(&r->vertices[r->vertex_count + t], &p->vertices[k->first + k->count - 3 - t],
                       3 * sizeof *p->vertices);
            r->vertex_count += k->count;
        }
    }
}

int dhdr_native_picture_indexed(DhdrNative *n, const DhdrPicture *p, const uint32_t *indices, uint32_t index_count,
                                DhdrPictureResult *out)
{
    memset(out, 0, sizeof *out);
    if (!n || !p || !ensure_gl_objects(n)) return 0;
    int times = gl_times();
    /* The phases are always timed (three clock reads a picture), for the slow field line. */
    uint64_t t0 = SDL_GetTicksNS(), t1 = 0, t2 = 0, t3 = 0;
    slow_field_check(t0);
    uint64_t uploads0, upload_ns0, upload_bytes0;
    dhdr_tex_upload_stats(&uploads0, &upload_ns0, &upload_bytes0);
    g_cost.start = t0;
    uint64_t program_changes = 0, texture_binds = 0;
    /* Textured draws this renderer still cannot give a texture to, by what it would have
       sampled: a conversion, its own target (or a surface with nothing in it), or a decoded
       texture that would not upload. They come out flat, and these say which to do next. */
    uint64_t flat_convert = 0, flat_surface = 0, flat_other = 0;
    /* The last conversion run and its texture (run_convert), for a run of draws sampling it. */
    int32_t last_convert = -1;
    GLuint converted = 0;
    uint32_t converted_w = 0, converted_h = 0;
    uint64_t out_converts = 0;
    static Reversed reversed;
    reverse_draws(n, p, indices, index_count, &reversed);
    size_t vertex_bytes = (size_t)p->vertex_count * sizeof(DhdrVertex);
    ensure_buffer(GL_ARRAY_BUFFER, n->vbo, &n->vbo_bytes, vertex_bytes + reversed.vertex_count * sizeof(DhdrVertex));
    if (p->vertex_count) glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)vertex_bytes, p->vertices);
    if (reversed.vertex_count)
        glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)vertex_bytes,
                        (GLsizeiptr)(reversed.vertex_count * sizeof(DhdrVertex)), reversed.vertices);
    if (indices && index_count) {
        size_t index_bytes = (size_t)index_count * sizeof(uint32_t);
        ensure_buffer(GL_ELEMENT_ARRAY_BUFFER, n->ibo, &n->ibo_bytes, index_bytes + reversed.index_count * sizeof(uint32_t));
        glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, (GLsizeiptr)index_bytes, indices);
        if (reversed.index_count)
            glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, (GLintptr)index_bytes,
                            (GLsizeiptr)(reversed.index_count * sizeof(uint32_t)), reversed.indices);
    }
    glBindVertexArray(n->vao);
    glEnable(GL_SCISSOR_TEST);
    glActiveTexture(GL_TEXTURE0);
    GLint bound_fbo = -1;
    GLuint bound_program = 0, bound_texture = 0, bound_sampler = 0;
    uint32_t held_mask = 16;                      /* no colour mask sent yet (a mask is 0..15) */
    int32_t held_scissor[4] = { -1, -1, -1, -1 }; /* nor a scissor (its values are never negative) */
    DepthState held_depth = { 0, 0, 0 };          /* the test off, as set right below */
    BlendState held_blend = { 0, 0, 0, 0 };
    uint32_t held_fix = 0xFFFFFFFFu;              /* no blend constant sent yet */
    int held_fix_set = 0;
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    for (uint32_t i = 0; i < p->command_count; i++) {
        const DhdrCommand *k = &p->commands[i];
        if (k->kind == DHDR_CMD_COPY || k->kind == DHDR_CMD_IMPORT || k->kind == DHDR_CMD_ALPHA_UPLOAD) {
            if (k->kind == DHDR_CMD_COPY) copy_surface(n, k, out);
            else if (k->kind == DHDR_CMD_IMPORT) import_pixels(n, k, out);
            else {
                alpha_upload(n, k);
                /* It sets the depth test, the blend, the mask and the program its own way. */
                held_depth.on = 0; held_depth.write = 0;
                held_blend.on = 0;
                held_mask = 15;
                bound_program = 0;
                bound_texture = 0;
                glEnable(GL_SCISSOR_TEST);
            }
            /* All three bind framebuffers of their own and change the scissor, and may remake
               the scratch texture a draw sampling its own target left bound. A conversion is run
               again after them, as render.rs does. */
            bound_fbo = -1;
            bound_texture = 0;
            last_convert = -1;
            held_scissor[0] = -1;
            continue;
        }
        if (k->kind != DHDR_CMD_DRAW || !k->count) continue;
        if (k->target < 0 || (uint32_t)k->target >= DHDR_NATIVE_SURFACE_COUNT) continue;
        NativeSurface *t = &n->surfaces[k->target];
        if (!t->texture || t->depth) continue;
        if (bound_fbo != (GLint)t->fbo) {
            glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
            glViewport(0, 0, (GLsizei)t->width, (GLsizei)t->height);
            bound_fbo = (GLint)t->fbo;
        }
        /* The Z this draw tests against (depth_key bit 3 says it has one, native_core.inc), as
           this colour surface's depth attachment. A colour surface keeps the one it was last
           given, so a run of draws sharing a Z attaches it once. */
        NativeSurface *z = draw_depth(n, k);
        GLuint want_z = z ? z->texture : 0;
        if (t->depth_attached != want_z) {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, want_z, 0);
            t->depth_attached = want_z;
        }
        uint32_t target_scale = t->scale;
        int32_t left = clampi(k->scissor[0] * (int32_t)target_scale, 0, (int32_t)t->width);
        int32_t top = clampi(k->scissor[1] * (int32_t)target_scale, 0, (int32_t)t->height);
        int32_t right = clampi(k->scissor[2] * (int32_t)target_scale, 0, (int32_t)t->width);
        int32_t bottom = clampi(k->scissor[3] * (int32_t)target_scale, 0, (int32_t)t->height);
        if (right <= left || bottom <= top) continue;
        int32_t cut[4] = { left, (int32_t)t->height - bottom, right - left, bottom - top };
        if (memcmp(held_scissor, cut, sizeof cut)) {
            glScissor(cut[0], cut[1], cut[2], cut[3]);
            memcpy(held_scissor, cut, sizeof cut);
        }
        /* Where this draw's texture comes from, and with it the pipeline, in the order render.rs
           tries: the surface it names, else a replacement (flag 2048) through one of the eight
           samplers, else its decoded texture. A draw sampling its own target needs the copy
           sample_target makes and is not drawn textured here; nor is one whose texture is a
           conversion, nor one whose texture could not be uploaded. Those lose the texture bit
           and come out flat, as every textured draw did in step 1. */
        uint32_t flags = k->constants.flags;
        unsigned key = PROGRAM_TEXELS;
        GLuint texture = 0, sampler = 0;
        uint32_t tw_texels = 1, th_texels = 1, source_scale = 1;
        if (k->texture_surface >= 0 && (uint32_t)k->texture_surface < DHDR_NATIVE_SURFACE_COUNT &&
            !(flags & 2048u)) {
            NativeSurface *src = &n->surfaces[k->texture_surface];
            if (src->texture && !src->depth) {
                key = 0;   /* the float path: a surface's RGBA8, fetched texel by texel */
                texture = src->texture;
                if (k->texture_surface == k->target) {
                    /* Its own target: reading and writing one texture at once is undefined, so
                       the part it can reach is copied first (surfaces.rs's sample_target). */
                    texture = sample_target(n, t, &k->constants, out);
                    /* sample_target may have remade the scratch texture (a deleted name can come
                       back for the new one, unbound) and bound its framebuffer. */
                    bound_fbo = -1;
                    bound_texture = 0;
                }
                tw_texels = src->width; th_texels = src->height;
                source_scale = src->scale ? src->scale : 1;
            }
        } else if ((flags & 2048u) && k->replacement && (texture = dhdr_tex_gl(k->replacement))) {
            key = 0;    /* the sampled path: the sampler does the filtering, so no BILINEAR */
            dhdr_tex_size(k->replacement, &tw_texels, &th_texels, NULL);
            sampler = dhdr_tex_sampler((flags >> 5 & 1) | (unsigned)(k->constants.wrap[0] != 0) << 1 |
                                       (unsigned)(k->constants.wrap[1] != 0) << 2);
        } else if (!(flags & 2048u) && k->texture_surface < 0 && k->convert < 0 && k->texture &&
                   (texture = dhdr_tex_gl(k->texture))) {
            dhdr_tex_size(k->texture, &tw_texels, &th_texels, NULL);
            if (flags & 32u) key |= PROGRAM_BILINEAR;
        } else if (!(flags & 2048u) && k->texture_surface < 0 && k->convert >= 0) {
            /* A conversion, run again for each run of draws (render.rs's last_convert rule: the
               surfaces it reads may have been drawn into since). The float path samples it as
               it samples a surface. */
            if (k->convert != last_convert) {
                converted = run_convert(n, p, k->convert, &converted_w, &converted_h);
                last_convert = k->convert;
                bound_program = 0;    /* the compute program, the units 0-8 and the image */
                bound_texture = 0;
                if (converted) out_converts++;
            }
            if (converted) {
                key = 0;
                texture = converted;
                tw_texels = converted_w; th_texels = converted_h;
            }
        }
        if (k->convert < 0) last_convert = -1;
        if (!texture) {
            if (flags & 1u) {   /* what is still missing, so the next round knows which to do */
                if (k->convert >= 0) flat_convert++;
                else if (k->texture_surface >= 0) flat_surface++;
                else flat_other++;
            }
            key = PROGRAM_TEXELS;
            texture = dhdr_tex_dummy_texels();
            tw_texels = th_texels = 1;
            source_scale = 1;
            flags &= ~(1u | 2048u);
        }
        if (flags & 64u) key |= PROGRAM_ALPHA_TEST;
        /* The scale word render.rs builds: target | source << 8 | min(source, target) << 16.
           The source scale is the sampled surface's, and 1 for a decoded texture or a
           replacement; S and E both being 1 is what folds the shader's texel arithmetic away. */
        uint32_t resolution = source_scale < t->scale ? source_scale : t->scale;
        if (source_scale == 1 && resolution == 1) key |= PROGRAM_SCALE_ONE;
        if (!k->constants.wrap[0] && !k->constants.wrap[1]) key |= PROGRAM_WRAP_REPEAT;
        DrawProgram *d = draw_program(n, key);
        if (!d) continue;
        if (d->u_texels_size >= 0 &&
            (d->h_texels_size[0] != (int32_t)tw_texels || d->h_texels_size[1] != (int32_t)th_texels)) {
            if (bound_program != d->id) { glUseProgram(d->id); bound_program = d->id; program_changes++; }
            glUniform2i(d->u_texels_size, (GLint)tw_texels, (GLint)th_texels);
            d->h_texels_size[0] = (int32_t)tw_texels; d->h_texels_size[1] = (int32_t)th_texels;
        }
        if (bound_program != d->id) {
            glUseProgram(d->id);
            bound_program = d->id;
            program_changes++;
        }
        if (bound_texture != texture) {
            glBindTexture(GL_TEXTURE_2D, texture);
            bound_texture = texture;
            texture_binds++;
        }
        if (bound_sampler != sampler) { glBindSampler(0, sampler); bound_sampler = sampler; }
        uint32_t scale_word = target_scale | source_scale << 8 | resolution << 16;
        float tw = (float)t->width / (float)target_scale, th = (float)t->height / (float)target_scale;
        DrawProgram *w = d;
        if (!w->held || w->h_target[0] != tw || w->h_target[1] != th) {
            glUniform2f(d->u_target_size, tw, th);
            w->h_target[0] = tw; w->h_target[1] = th;
        }
        if (!w->held || w->h_flags != flags) { glUniform1ui(d->u_flags, flags); w->h_flags = flags; }
        if (!w->held || w->h_scale != scale_word) { glUniform1ui(d->u_scale, scale_word); w->h_scale = scale_word; }
        if (!w->held || w->h_atst != k->constants.atst) {
            glUniform1ui(d->u_atst, k->constants.atst);
            w->h_atst = k->constants.atst;
        }
        if (!w->held || w->h_aref != k->constants.aref) {
            glUniform1ui(d->u_aref, k->constants.aref);
            w->h_aref = k->constants.aref;
        }
        if (!w->held || memcmp(w->h_fog, k->constants.fog, sizeof w->h_fog)) {
            glUniform4f(d->u_fog, k->constants.fog[0], k->constants.fog[1], k->constants.fog[2],
                        k->constants.fog[3]);
            memcpy(w->h_fog, k->constants.fog, sizeof w->h_fog);
        }
        if (!w->held || w->h_alpha_scale != k->constants.alpha_scale) {
            glUniform1f(d->u_alpha_scale, k->constants.alpha_scale);
            w->h_alpha_scale = k->constants.alpha_scale;
        }
        if (!w->held || w->h_texa_alpha != k->constants.texa_alpha) {
            glUniform1f(d->u_texa_alpha, k->constants.texa_alpha);
            w->h_texa_alpha = k->constants.texa_alpha;
        }
        if (!w->held || memcmp(w->h_tex_offset, k->constants.tex_offset, sizeof w->h_tex_offset)) {
            glUniform2i(d->u_tex_offset, k->constants.tex_offset[0], k->constants.tex_offset[1]);
            memcpy(w->h_tex_offset, k->constants.tex_offset, sizeof w->h_tex_offset);
        }
        if (!w->held || memcmp(w->h_tex_size, k->constants.tex_size, sizeof w->h_tex_size)) {
            glUniform2ui(d->u_tex_size, k->constants.tex_size[0], k->constants.tex_size[1]);
            memcpy(w->h_tex_size, k->constants.tex_size, sizeof w->h_tex_size);
        }
        if (!w->held || w->h_wrap[0] != k->constants.wrap[0] || w->h_wrap[1] != k->constants.wrap[1]) {
            glUniform2ui(d->u_wrap, k->constants.wrap[0], k->constants.wrap[1]);
            w->h_wrap[0] = k->constants.wrap[0]; w->h_wrap[1] = k->constants.wrap[1];
        }
        if (!w->held || memcmp(w->h_region, k->constants.region, sizeof w->h_region)) {
            glUniform4ui(d->u_region, k->constants.region[0], k->constants.region[1], k->constants.region[2],
                         k->constants.region[3]);
            memcpy(w->h_region, k->constants.region, sizeof w->h_region);
        }
        w->held = 1;
        /* The depth test and the blend (pipelines.rs's key), then the draw — one, or the two
           render.rs splits a blend on As into where there is no dual-source blending, which this
           device has none of (dhdr_device_features offers none). */
        uint32_t blend_key = k->blend_key;
        uint32_t src = blend_key >> 1 & 31, dst = blend_key >> 6 & 31;
        int blends = (blend_key & 1) != 0;
        GLenum op = blends ? blend_op(blend_key >> 11 & 7) : GL_FUNC_ADD;
        if (blends && !op) continue;   /* operations 6 and 7 have no pipeline in the Rust path either */
        int uses_as = blend_uses_as(blend_key);
        DepthState zs = { z != NULL, z && (k->depth_key & 4), (uint32_t)(k->depth_key & 3) };
        BlendState bs = { blends, blend_factor(src), blend_factor(dst), op };
        if (blends && (src == 14 || src == 15 || dst == 14 || dst == 15) &&
            (!held_fix_set || held_fix != k->blend_fix)) {
            float fix = (float)k->blend_fix / 128.0f;   /* all four channels (render.rs set_state) */
            glBlendColor(fix, fix, fix, fix);
            held_fix = k->blend_fix;
            held_fix_set = 1;
        }
        if (!uses_as) {
            set_depth(&held_depth, zs);
            set_blend(&held_blend, bs);
            set_mask(&held_mask, k->color_mask);
            set_alpha_div(d, 255.0f);
            issue(indices, index_count, k->first, k->count);
            out->draws++;
        } else {
            if (k->color_mask & 7) {   /* the colour, blended, with As / 128 as its source alpha */
                set_depth(&held_depth, zs);
                set_blend(&held_blend, bs);
                set_mask(&held_mask, (uint32_t)k->color_mask & 7);
                set_alpha_div(d, 128.0f);
                issue(indices, index_count, k->first, k->count);
                out->draws++;
            }
            if (k->color_mask & 8) {   /* then the alpha channel alone, unblended */
                /* No write, and EQUAL where the first pass wrote Z; for GREATER with writes in
                   reverse triangle order (reverse_draws), so a later triangle at an equal Z loses
                   as it did in the first pass. */
                DepthState second = { z != NULL, 0,
                                      z ? ((k->depth_key & 4) ? 4u : (uint32_t)(k->depth_key & 3)) : 0u };
                BlendState none = { 0, GL_ONE, GL_ZERO, GL_FUNC_ADD };
                set_depth(&held_depth, second);
                set_blend(&held_blend, none);
                set_mask(&held_mask, 8);
                set_alpha_div(d, 255.0f);
                issue(indices, index_count, reversed.valid && reversed.first[i] != UINT32_MAX ? reversed.first[i] : k->first, k->count);
                out->draws++;
                out->two_pass_draws++;
            }
        }
    }
    glBindSampler(0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    t1 = phase_end(times);
    /* Textures released on any thread since the last picture: their GL objects are this
       thread's to delete (textures_gl.h). */
    dhdr_tex_collect();
    t2 = phase_end(times);
    int shown = p->movie_on ? scan_out_movie(n, p->movie_width, p->movie_height, p->movie) : scan_out(n, p);
    t3 = phase_end(times);
    {
        uint64_t uploads, upload_ns, upload_bytes;
        dhdr_tex_upload_stats(&uploads, &upload_ns, &upload_bytes);
        g_cost.uploads = uploads - uploads0;
        g_cost.upload_ns = upload_ns - upload_ns0;
        g_cost.upload_bytes = upload_bytes - upload_bytes0;
        g_cost.draws_ns = t1 - t0; g_cost.collect_ns = t2 - t1; g_cost.scan_ns = t3 - t2;
        g_cost.draws = out->draws; g_cost.copies = out->copies; g_cost.imports = out->imports;
        g_cost.converts = (uint32_t)out_converts;
        g_cost.end = t3;
        g_last = g_cost;
        memset(&g_cost, 0, sizeof g_cost);
        g_cost_fields++;
    }
    if (times) {
        /* A movie frame is no scanout: its pictures are summed under the form in use anyway. */
        if (report_times(t1 - t0, t3 - t2, t2 - t1, out->draws, program_changes, texture_binds,
                         flat_convert, flat_surface, flat_other, scanout_names[n->scanout_mode]) &&
            n->scanout_cycle)
            n->scanout_mode = (n->scanout_mode + 1) % SCAN_FORMS;
    }
    out->shown = shown != 0;
    out->display_width = n->display_width;
    out->display_height = n->display_height;
    return 1;
}

int dhdr_native_picture(DhdrNative *n, const DhdrPicture *p, DhdrPictureResult *out)
{
    return dhdr_native_picture_indexed(n, p, NULL, 0, out);
}

/* ---- presentation (the display texture into the window, as render_gl.c's exact present) ---- */

static int native_present_draw(DhdrNative *n, const DhdrPresentRect *r);

int dhdr_native_present(DhdrNative *n, DhdrSurface *s, const DhdrPresentRect *r)
{
    if (!n || !s || !r) return -1;
    if (!r->window_w || !r->window_h) return 0;
    if (!dhdr_gl_make_current(s)) return -1;
    g_last.present_began = SDL_GetTicksNS();
    int shown = native_present_draw(n, r);
    g_last.present_ended = SDL_GetTicksNS();
    return shown;
}

static int native_present_draw(DhdrNative *n, const DhdrPresentRect *r)
{
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, (GLsizei)r->window_w, (GLsizei)r->window_h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!n->display_texture) return 1;   /* nothing scanned out yet: the cleared window is shown */
    int32_t cut_left = r->cut[0] < 0 ? 0 : r->cut[0];
    int32_t cut_top = r->cut[1] < 0 ? 0 : r->cut[1];
    int32_t cut_right = r->cut[2] > (int32_t)r->window_w ? (int32_t)r->window_w : r->cut[2];
    int32_t cut_bottom = r->cut[3] > (int32_t)r->window_h ? (int32_t)r->window_h : r->cut[3];
    if (cut_right <= cut_left || cut_bottom <= cut_top) return 1;
    float left = r->rect[0] / (float)r->window_w * 2.0f - 1.0f;
    float top = 1.0f - r->rect[1] / (float)r->window_h * 2.0f;
    float right = (r->rect[0] + r->rect[2]) / (float)r->window_w * 2.0f - 1.0f;
    float bottom = 1.0f - (r->rect[1] + r->rect[3]) / (float)r->window_h * 2.0f;
    glEnable(GL_SCISSOR_TEST);
    glScissor(cut_left, (int32_t)r->window_h - cut_bottom, cut_right - cut_left, cut_bottom - cut_top);
    glUseProgram(n->present_program);
    glUniform4f(n->u_rect, left, top, right, bottom);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, n->display_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, n->point_filter ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, n->point_filter ? GL_NEAREST : GL_LINEAR);
    glBindVertexArray(n->vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_SCISSOR_TEST);
    return 1;
}

/* ---- reading the displayed picture back ---- */

int dhdr_native_read_begin(DhdrNative *n, uint32_t *w, uint32_t *h)
{
    if (!n || !n->display_texture || !gl_ready()) { *w = *h = 0; return 0; }
    *w = n->display_width; *h = n->display_height;
    return 1;
}

int dhdr_native_read_finish(DhdrNative *n, uint32_t *pixels)
{
    if (!n || !n->display_texture || !gl_ready()) return 0;
    glBindFramebuffer(GL_FRAMEBUFFER, n->display_fbo);
    glReadPixels(0, 0, (GLsizei)n->display_width, (GLsizei)n->display_height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return 1;
}

/* ---- the Z probe (finding 097, zread.rs) ----

   The game reads a small patch of its Z buffer every field to hide the lens flare behind the
   scenery. After a picture, native_core asks for the patch (at most 64 x 64 GS pixels, each the
   texel at the centre of its scaled block) from the Z surface just drawn; a compute pass copies
   it into a buffer and a fence marks the end. A later picture takes the newest copy whose fence
   has passed (never waiting), so the answer is a field or two late, as on every other target.
   The words are depth32float bit patterns, which native_core turns into GS Z.

   The Y flip: a surface texture holds the GS picture upside down (native_vertex), so GS row y is
   texture row (height - 1 - y). */
static const char *zread_source =
    "#version 430 core\n"
    "layout(local_size_x = 8, local_size_y = 8) in;\n"
    "layout(binding = 0) uniform sampler2D zsource;\n"
    "layout(std430, binding = 0) writeonly buffer ZOut { uint words[]; };\n"
    "uniform uvec4 rect;\n"     /* x, y, w, h in GS pixels */
    "uniform uint zscale;\n"
    "void main() {\n"
    "    uvec2 id = gl_GlobalInvocationID.xy;\n"
    "    if (id.x >= rect.z || id.y >= rect.w) return;\n"
    "    uvec2 p = (rect.xy + id) * zscale + zscale / 2u;\n"
    "    int rows = textureSize(zsource, 0).y;\n"
    "    float d = texelFetch(zsource, ivec2(int(p.x), rows - 1 - int(p.y)), 0).r;\n"
    "    words[id.y * rect.z + id.x] = floatBitsToUint(d);\n"
    "}\n";

/* The probe's program, made once (prelink_programs makes it at start-up). */
static int ensure_zread(DhdrNative *n)
{
    static int failed;
    if (n->zread_program) return 1;
    if (failed) return 0;
    n->zread_program = link_program(NULL, NULL, NULL, zread_source);
    if (!n->zread_program) { failed = 1; return 0; }
    n->zread_rect = glGetUniformLocation(n->zread_program, "rect");
    n->zread_scale = glGetUniformLocation(n->zread_program, "zscale");
    return 1;
}

int dhdr_native_zread_start(DhdrNative *n, uint32_t index, const uint32_t rect[4], uint64_t tag)
{
    if (!n || index >= DHDR_NATIVE_SURFACE_COUNT || !rect || !n->ready) return 0;
    NativeSurface *s = &n->surfaces[index];
    if (!s->texture || !s->depth) return 0;
    uint32_t scale = s->scale ? s->scale : 1;
    uint32_t cols = s->width / scale, rows = s->height / scale;
    uint32_t x = rect[0], y = rect[1], w = rect[2], h = rect[3];
    if (x >= cols || y >= rows) return 0;
    if (w > ZREAD_MAX) w = ZREAD_MAX;
    if (h > ZREAD_MAX) h = ZREAD_MAX;
    if (w > cols - x) w = cols - x;
    if (h > rows - y) h = rows - y;
    if (!w || !h) return 0;
    if (!ensure_zread(n)) return 0;
    int slot = -1;
    for (int i = 0; i < ZREAD_SLOTS && slot < 0; i++)
        if (!n->zslots[i].fence) slot = i;
    if (slot < 0) return 0;   /* three copies in flight: this field's probe is skipped */
    if (!n->zslots[slot].buffer) {
        glGenBuffers(1, &n->zslots[slot].buffer);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, n->zslots[slot].buffer);
        glBufferData(GL_SHADER_STORAGE_BUFFER, ZREAD_MAX * ZREAD_MAX * 4, NULL, GL_STREAM_READ);
    }
    glUseProgram(n->zread_program);
    glUniform4ui(n->zread_rect, x, y, w, h);
    glUniform1ui(n->zread_scale, scale);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s->texture);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, n->zslots[slot].buffer);
    glDispatchCompute((w + 7) / 8, (h + 7) / 8, 1);
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
    n->zslots[slot].fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    n->zslots[slot].rect[0] = x; n->zslots[slot].rect[1] = y;
    n->zslots[slot].rect[2] = w; n->zslots[slot].rect[3] = h;
    n->zslots[slot].tag = tag;
    return 1;
}

int dhdr_native_zread_take(DhdrNative *n, uint32_t *out, uint32_t cap, uint32_t rect_out[4], uint64_t *tag_out)
{
    if (!n || !n->ready) return 0;
    int done[ZREAD_SLOTS] = { 0 }, newest = -1;
    for (int i = 0; i < ZREAD_SLOTS; i++) {
        if (!n->zslots[i].fence) continue;
        GLenum state = glClientWaitSync(n->zslots[i].fence, 0, 0);
        done[i] = state == GL_ALREADY_SIGNALED || state == GL_CONDITION_SATISFIED;
        if (done[i] && (newest < 0 || n->zslots[i].tag > n->zslots[newest].tag)) newest = i;
    }
    if (newest < 0) return 0;
    int taken = 0;
    uint32_t count = n->zslots[newest].rect[2] * n->zslots[newest].rect[3];
    if (out && count <= cap) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, n->zslots[newest].buffer);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)count * 4, out);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        memcpy(rect_out, n->zslots[newest].rect, sizeof n->zslots[newest].rect);
        *tag_out = n->zslots[newest].tag;
        taken = 1;
    }
    /* The newest and every older finished copy are used up. */
    for (int i = 0; i < ZREAD_SLOTS; i++)
        if (done[i] && n->zslots[i].tag <= n->zslots[newest].tag) {
            glDeleteSync(n->zslots[i].fence);
            n->zslots[i].fence = 0;
        }
    return taken;
}
