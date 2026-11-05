/* The Switch's native renderer, NX2 steps 1 and 2 (finding 144): textured GS geometry on
   OpenGL 4.3 core (DHD_RENDER=native; dhd.ini "renderer = native"). It stands in
   for the Rust renderer's native half (port/render/src/native, native.wgsl) the same way
   render_gl.c stands in for its device and presentation: the command loop, the draw pipeline, the
   scanout and the present pass, all translated line for line from native.wgsl and
   port/render/src/native/render.rs. The CPU half (arenas, surfaces as native_core.inc sees them,
   the picture it builds) stays portable C (native_core.inc, gpu_rust_native.inc,
   gpu_rust_shared.inc), unchanged on every target.

   What steps 1 and 2 draw: every DHDR_CMD_DRAW, with its vertex colour, fog, alpha test and —
   step 2 — its texture: a decoded texture (textures.c's R32UI words, fetched by the shader's
   texel path, native.wgsl's native_pixel_u) or a replacement, a texture pack's or the sharp
   font's (RGBA8 mips through one of the eight samplers, native_pixel). A draw that samples a
   surface or a conversion instead (texture_surface, convert) keeps step 1's flat colour: the
   host clears its texture bit. Still missing, all step 3: the depth test, blending, the texture
   copies (DHDR_CMD_COPY, IMPORT and ALPHA_UPLOAD are no-ops) and render-target sampling.
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

typedef struct NativeSurface {
    GLuint texture, fbo;
    uint32_t width, height;    /* scaled pixels, as dhdr_native_surface's w, h */
    uint32_t scale;            /* the resolution scale this surface's texture is at */
    int depth;                 /* a depth surface (GL_DEPTH_COMPONENT32F): never drawn into here */
} NativeSurface;

/* One draw pipeline and its uniforms (native.wgsl's Native block, which GL has as loose
   uniforms): the two differ only in the texture they sample, so they share every location name.
   `texels` reads a decoded texture's R32UI words (native_pixel_u), `sampled` a replacement's
   RGBA8 mips (native_pixel). An untextured draw uses `texels` with the texture bit cleared. */
typedef struct DrawProgram {
    GLuint id;
    GLint u_target_size, u_flags, u_scale, u_atst, u_aref, u_fog;
    GLint u_alpha_scale, u_texa_alpha, u_tex_offset, u_tex_size, u_wrap, u_region;
    /* What the program already holds. A GS picture is thousands of draws whose state mostly
       repeats, and a uniform the program already has is a driver call for nothing; `held` says
       the fields below are what was last sent to this program (uniforms are per program, so
       each has its own copy). */
    int held;
    float h_target[2], h_fog[4], h_alpha_scale, h_texa_alpha;
    uint32_t h_flags, h_scale, h_atst, h_aref, h_tex_size[2], h_wrap[2], h_region[4];
    int32_t h_tex_offset[2];
} DrawProgram;

struct DhdrNative {
    DrawProgram texels, sampled;
    GLuint scanout_program;
    GLint u_da, u_db, u_ds;
    GLuint present_program;
    GLint u_rect;
    GLuint vao, vbo, ibo;
    size_t vbo_bytes, ibo_bytes;
    int ready;                 /* the GL objects above are built (ensure_gl_objects) */
    NativeSurface surfaces[DHDR_NATIVE_SURFACE_COUNT];
    GLuint display_texture, display_fbo;
    uint32_t display_width, display_height;
    int point_filter;
};

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
    "    gl_Position = vec4(position.xy / target_size * vec2(2.0, -2.0) + vec2(-1.0, 1.0), position.z, 1.0);\n"
    "    v_stqf = stqf;\n"
    "    v_color = vec4(color_in);\n"
    "}\n";

/* native_pixel (and native_pixel_u with TEXELS): texture_uv, the texel fetch or the replacement
   sample, combine() and the output, the same arithmetic as native.wgsl. The uniform block is
   loose uniforms here, and native_spec is always 0 (the generic pipeline: no specialisation
   constants in GL), so draw_flags() is `flags`, wrap_mode() is `wrap`, alpha_test() is `atst`
   and texture_sized() is false — load_texels asks the texture for its size, and bilinear_u's
   one-gather fast path, which needs texture_sized(), is never taken: its four loads are.

   gl_FragCoord is declared origin_upper_left so that, with the vertex stage's Y flip, it counts
   GS pixels from the top exactly as WGSL's @builtin(position) does (fetch's flag-1024 snap).

   Blending, depth and sampling a surface or a conversion stay step 3: the host clears the
   texture bit of a draw it cannot give a texture to, so such a draw comes out flat as it did in
   step 1. */
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
    "uniform float alpha_scale;\n"
    "uniform float texa_alpha;\n"
    "uniform ivec2 tex_offset;\n"
    "uniform uvec2 tex_size;\n"
    "uniform uvec2 wrap;\n"
    "uniform uvec4 region;\n"
    "#ifdef TEXELS\n"
    "uniform usampler2D native_texels;\n"
    "#else\n"
    "uniform sampler2D native_texture;\n"
    "#endif\n"
    /* CLAMP modes: repeat, clamp, region clamp, region repeat (MINU the mask, MAXU the fix). */
    "uint wrap_coordinate(int c, uint mode, uint size, uint low, uint high) {\n"
    "    if (mode == 0u) return uint(c) & (size - 1u);\n"
    "    if (mode == 1u) return uint(clamp(c, 0, int(size) - 1));\n"
    "    if (mode == 2u) return uint(clamp(c, int(low), int(high)));\n"
    "    return (uint(c) & low) | high;\n"
    "}\n"
    /* The texture pixel holding GS texel c (S the texture's scale, E the draw's). */
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
    "#ifdef TEXELS\n"
    "uint load_texels(ivec2 xy) {\n"
    "    ivec2 size = textureSize(native_texels, 0);\n"
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
    "        return mix(mix(texel_u(b), texel_u(b + ivec2(1, 0)), f.x),\n"
    "                   mix(texel_u(b + ivec2(0, 1)), texel_u(b + ivec2(1, 1)), f.x), f.y);\n"
    "    }\n"
    "    return texel_u(ivec2(floor(uv)));\n"
    "}\n"
    "#else\n"
    /* A replacement (flag 2048): the mips sampled, region clamp applied half a texel inside. */
    "vec4 replacement(vec2 uv_in, vec2 across, vec2 down) {\n"
    "    vec2 uv = uv_in;\n"
    "    vec2 size = vec2(textureSize(native_texture, 0));\n"
    "    vec2 gs_size = vec2(tex_size);\n"
    "    vec2 half_texel = gs_size / size * 0.5;\n"
    "    if (wrap.x == 2u) uv.x = clamp(uv.x, float(region.x) + half_texel.x, float(region.y) + 1.0 - half_texel.x);\n"
    "    if (wrap.y == 2u) uv.y = clamp(uv.y, float(region.z) + half_texel.y, float(region.w) + 1.0 - half_texel.y);\n"
    "    vec4 t = textureGrad(native_texture, uv / gs_size, across / gs_size, down / gs_size);\n"
    "    return vec4(t.rgb * 255.0, t.a * alpha_scale);\n"
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
    "    if ((flags & 64u) != 0u) {\n"
    "        uint a = uint(color.a);\n"
    "        bool passed = atst == 1u || (atst == 2u && a < aref) || (atst == 3u && a <= aref) ||\n"
    "                      (atst == 4u && a == aref) || (atst == 5u && a >= aref) || (atst == 6u && a > aref) ||\n"
    "                      (atst == 7u && a != aref);\n"
    "        if (passed == ((flags & 128u) != 0u)) discard;\n"
    "    }\n"
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
    "        t = replacement(uv, across, down);\n"
    "#endif\n"
    "    }\n"
    "    vec4 color = combine(t);\n"
    "    frag_color = vec4(color.rgb / 255.0, clamp(color.a / 255.0, 0.0, 1.0));\n"
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
    glUseProgram(d->id);
    glUniform1i(glGetUniformLocation(d->id, sampler_name), unit);
    return 1;
}

static int ensure_gl_objects(DhdrNative *n)
{
    if (n->ready) return 1;
    if (!gl_ready()) return 0;
    n->texels.id = link_program(native_vertex_source, "#version 430 core\n#define TEXELS 1\n",
                                native_fragment_source, NULL);
    n->sampled.id = link_program(native_vertex_source, "#version 430 core\n", native_fragment_source, NULL);
    n->scanout_program = link_program(NULL, NULL, NULL, scanout_source);
    n->present_program = link_program(present_vertex_source, NULL, present_fragment_source, NULL);
    if (!n->texels.id || !n->sampled.id || !n->scanout_program || !n->present_program) return 0;
    find_draw_uniforms(&n->texels, "native_texels", 0);
    find_draw_uniforms(&n->sampled, "native_texture", 0);
    n->u_da = glGetUniformLocation(n->scanout_program, "da");
    n->u_db = glGetUniformLocation(n->scanout_program, "db");
    n->u_ds = glGetUniformLocation(n->scanout_program, "ds");
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
    fprintf(stderr, "gs native: GL native renderer ready (step 2: textured geometry; no depth, "
                    "no blending, no render-target sampling yet)\n");
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
        glDeleteBuffers(1, &n->vbo);
        glDeleteBuffers(1, &n->ibo);
        glDeleteVertexArrays(1, &n->vao);
        glDeleteProgram(n->texels.id);
        glDeleteProgram(n->sampled.id);
        glDeleteProgram(n->scanout_program);
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
    uint32_t nw = s->texture && s->width > w ? s->width : (w ? w : 1);
    uint32_t nh = s->texture && s->height > h ? s->height : (h ? h : 1);
    GLuint texture = 0, fbo = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, depth ? GL_DEPTH_COMPONENT32F : GL_RGBA8, (GLsizei)nw, (GLsizei)nh);
    if (!depth) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, depth ? GL_DEPTH_ATTACHMENT : GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (!depth) { glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT); }
    else { glClearDepthf(1.0f); glClear(GL_DEPTH_BUFFER_BIT); }
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
    size_out[0] = nw; size_out[1] = nh;
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
    glUseProgram(n->scanout_program);
    glUniform4ui(n->u_da, d[0], d[1], d[2], d[3]);
    glUniform4ui(n->u_db, d[4], d[5], d[6], d[7]);
    glUniform1ui(n->u_ds, d[8]);
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

/* ---- the picture: native_render's command loop, DRAW only (step 1 has no textures) ---- */

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
        const char *s = SDL_getenv("DHD_GL_TIMES");
        level = s ? atoi(s) : 0;
    }
    return level;
}

static uint64_t phase_end(int level)
{
    if (level >= 2) glFinish();
    return SDL_GetTicksNS();
}

static void report_times(uint64_t draws_ns, uint64_t scan_ns, uint64_t collect_ns, uint64_t draws,
                         uint64_t programs, uint64_t textures)
{
    static uint64_t pictures, a_draws_ns, a_scan_ns, a_collect_ns, a_draws, a_programs, a_textures;
    static uint64_t was_uploads, was_upload_ns, was_bytes, since;
    a_draws_ns += draws_ns; a_scan_ns += scan_ns; a_collect_ns += collect_ns;
    a_draws += draws; a_programs += programs; a_textures += textures;
    uint64_t now = SDL_GetTicksNS();
    if (!since) since = now;
    /* 300 pictures or five seconds, whichever comes first: at 60 fields a second that is a line
       every five seconds, and at three (the rate this was written to explain) it still is, rather
       than one line every hundred seconds. */
    if (++pictures < 300 && now - since < 5000000000ull) return;
    since = now;
    uint64_t uploads, upload_ns, bytes;
    dhdr_tex_upload_stats(&uploads, &upload_ns, &bytes);
    double n = (double)pictures;
    fprintf(stderr, "gs native: per picture over %u: draws %.2f ms (%.0f draws, %.0f program changes, "
                    "%.0f texture binds), scanout %.2f ms, collect %.2f ms | uploads %.1f (%.2f ms, "
                    "%.0f KiB)\n",
            (unsigned)pictures, a_draws_ns / n / 1e6, a_draws / n, a_programs / n, a_textures / n,
            a_scan_ns / n / 1e6, a_collect_ns / n / 1e6, (uploads - was_uploads) / n,
            (upload_ns - was_upload_ns) / n / 1e6, (bytes - was_bytes) / n / 1024.0);
    was_uploads = uploads; was_upload_ns = upload_ns; was_bytes = bytes;
    pictures = a_draws_ns = a_scan_ns = a_collect_ns = a_draws = a_programs = a_textures = 0;
}

int dhdr_native_picture_indexed(DhdrNative *n, const DhdrPicture *p, const uint32_t *indices, uint32_t index_count,
                                DhdrPictureResult *out)
{
    memset(out, 0, sizeof *out);
    if (!n || !p || !ensure_gl_objects(n)) return 0;
    int times = gl_times();
    uint64_t t0 = times ? SDL_GetTicksNS() : 0, t1 = 0, t2 = 0, t3 = 0;
    uint64_t program_changes = 0, texture_binds = 0;
    ensure_buffer(GL_ARRAY_BUFFER, n->vbo, &n->vbo_bytes, (size_t)p->vertex_count * sizeof(DhdrVertex));
    if (p->vertex_count) glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)((size_t)p->vertex_count * sizeof(DhdrVertex)),
                                        p->vertices);
    if (indices && index_count) {
        ensure_buffer(GL_ELEMENT_ARRAY_BUFFER, n->ibo, &n->ibo_bytes, (size_t)index_count * sizeof(uint32_t));
        glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, (GLsizeiptr)((size_t)index_count * sizeof(uint32_t)), indices);
    }
    glBindVertexArray(n->vao);
    glDisable(GL_DEPTH_TEST);   /* depth, blending: step 3 */
    glDisable(GL_BLEND);
    glEnable(GL_SCISSOR_TEST);
    glActiveTexture(GL_TEXTURE0);
    GLint bound_fbo = -1;
    GLuint bound_program = 0, bound_texture = 0, bound_sampler = 0;
    uint32_t held_mask = 16;                      /* no colour mask sent yet (a mask is 0..15) */
    int32_t held_scissor[4] = { -1, -1, -1, -1 }; /* nor a scissor (its values are never negative) */
    for (uint32_t i = 0; i < p->command_count; i++) {
        const DhdrCommand *k = &p->commands[i];
        if (k->kind != DHDR_CMD_DRAW || !k->count) continue;   /* COPY, IMPORT, ALPHA_UPLOAD: step 3 */
        if (k->target < 0 || (uint32_t)k->target >= DHDR_NATIVE_SURFACE_COUNT) continue;
        NativeSurface *t = &n->surfaces[k->target];
        if (!t->texture || t->depth) continue;
        if (bound_fbo != (GLint)t->fbo) {
            glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
            glViewport(0, 0, (GLsizei)t->width, (GLsizei)t->height);
            bound_fbo = (GLint)t->fbo;
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
        if (held_mask != (uint32_t)k->color_mask) {
            glColorMask((k->color_mask & 1) != 0, (k->color_mask & 2) != 0, (k->color_mask & 4) != 0,
                        (k->color_mask & 8) != 0);
            held_mask = k->color_mask;
        }
        /* The texture this draw samples, and with it the pipeline. A replacement (flag 2048) is
           sampled through one of the eight samplers; a decoded texture is fetched texel by texel.
           A draw that would sample a surface or a conversion (texture_surface, convert: step 3),
           and one whose texture could not be uploaded, loses its texture bit and comes out flat,
           as every textured draw did in step 1. */
        uint32_t flags = k->constants.flags;
        DrawProgram *d = &n->texels;
        GLuint texture = 0, sampler = 0;
        if ((flags & 2048u) && k->replacement && (texture = dhdr_tex_gl(k->replacement))) {
            d = &n->sampled;
            sampler = dhdr_tex_sampler((flags >> 5 & 1) | (unsigned)(k->constants.wrap[0] != 0) << 1 |
                                       (unsigned)(k->constants.wrap[1] != 0) << 2);
        } else if (!(flags & 2048u) && k->texture_surface < 0 && k->convert < 0 && k->texture &&
                   (texture = dhdr_tex_gl(k->texture))) {
            /* a decoded texture: n->texels, nothing to change */
        } else {
            texture = dhdr_tex_dummy_texels();
            flags &= ~(1u | 2048u);
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
        /* native.wgsl's scale word, as render.rs builds it: the target's scale, the sampled
           surface's (1 for a decoded texture or a replacement, the only sources step 2 reads)
           and the smaller of the two. */
        uint32_t source_scale = 1;
        uint32_t resolution = source_scale < target_scale ? source_scale : target_scale;
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
        if (indices && index_count) {
            glDrawElements(GL_TRIANGLES, (GLsizei)k->count, GL_UNSIGNED_INT,
                           (const void *)(uintptr_t)((size_t)k->first * sizeof(uint32_t)));
        } else {
            glDrawArrays(GL_TRIANGLES, (GLint)k->first, (GLsizei)k->count);
        }
        out->draws++;
    }
    glBindSampler(0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (times) t1 = phase_end(times);
    /* Textures released on any thread since the last picture: their GL objects are this
       thread's to delete (textures_gl.h). */
    dhdr_tex_collect();
    if (times) t2 = phase_end(times);
    int shown = p->movie_on ? scan_out_movie(n, p->movie_width, p->movie_height, p->movie) : scan_out(n, p);
    if (times) {
        t3 = phase_end(times);
        report_times(t1 - t0, t3 - t2, t2 - t1, out->draws, program_changes, texture_binds);
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

int dhdr_native_present(DhdrNative *n, DhdrSurface *s, const DhdrPresentRect *r)
{
    if (!n || !s || !r) return -1;
    if (!r->window_w || !r->window_h) return 0;
    if (!dhdr_gl_make_current(s)) return -1;
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

/* ---- the Z probe (finding 097): not supported before step 3's depth surfaces ---- */

int dhdr_native_zread_start(DhdrNative *n, uint32_t index, const uint32_t rect[4], uint64_t tag)
{ (void)n; (void)index; (void)rect; (void)tag; return 0; }
int dhdr_native_zread_take(DhdrNative *n, uint32_t *out, uint32_t cap, uint32_t rect_out[4], uint64_t *tag_out)
{ (void)n; (void)out; (void)cap; (void)rect_out; (void)tag_out; return 0; }
