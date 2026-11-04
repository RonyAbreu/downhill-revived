/* The Switch's native renderer, NX2 step 2's start, step 1 (finding 144): flat GS geometry on
   OpenGL 4.3 core, no textures yet (DHD_RENDER=native; dhd.ini "renderer = native"). It stands in
   for the Rust renderer's native half (port/render/src/native, native.wgsl) the same way
   render_gl.c stands in for its device and presentation: the command loop, the draw pipeline, the
   scanout and the present pass, all translated line for line from native.wgsl and
   port/render/src/native/render.rs. The CPU half (arenas, surfaces as native_core.inc sees them,
   the picture it builds) stays portable C (native_core.inc, gpu_rust_native.inc,
   gpu_rust_shared.inc), unchanged on every target.

   What step 1 draws: every DHDR_CMD_DRAW, untextured (the texture bit of a draw's flags is never
   read: a textured draw comes out flat, as flags & 1 == 0 would), with its vertex colour, fog and
   alpha test; no depth test, no blending, no texture copies (DHDR_CMD_COPY, IMPORT, ALPHA_UPLOAD
   are no-ops). Scanout and present are exact (native.wgsl's native_scanout, in a compute shader,
   and the same windowed quad render_gl.c's exact present uses), so the picture is in the right
   place and the right shape; it is flat-coloured menus, not exact pixels, until step 2 (textures)
   and step 3 (depth, blending, render targets).

   GL context: render_gl.c's, made current lazily on the first dhdr_present_exact or
   dhdr_native_present call (render_gl_ext.h). The native renderer's own GL calls (building
   surfaces, drawing a picture) happen earlier in the same field (gpu_rust_shared.inc's
   native_render, then native_present), on the same thread, before any present call has run yet on
   the very first field: this file makes the game's one surface current itself, the same way
   render_gl.c's present calls do. */
#include <glad/glad.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dhd_render.h"
#include "dhd_render_ext.h"
#include "render_gl_ext.h"

enum { DHDR_NATIVE_SURFACE_COUNT = 48 };

typedef struct NativeSurface {
    GLuint texture, fbo;
    uint32_t width, height;    /* scaled pixels, as dhdr_native_surface's w, h */
    uint32_t scale;            /* the resolution scale this surface's texture is at */
    int depth;                 /* a depth surface (GL_DEPTH_COMPONENT32F): never drawn into here */
} NativeSurface;

struct DhdrNative {
    GLuint draw_program;
    GLint u_target_size, u_flags, u_scale, u_atst, u_aref, u_fog;
    GLuint scanout_program;
    GLint u_da, u_db, u_ds;
    GLuint present_program;
    GLint u_rect;
    GLuint vao, vbo, ibo;
    size_t vbo_bytes, ibo_bytes;
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

/* native_pixel's combine(), the untextured path only (flags & 1 is never set here): fog, alpha
   test, premultiplied alpha (flag 512), the same arithmetic as native.wgsl. */
static const char *native_fragment_source =
    "#version 430 core\n"
    "in vec4 v_stqf;\n"
    "in vec4 v_color;\n"
    "out vec4 frag_color;\n"
    "uniform uint flags;\n"
    "uniform uint atst;\n"
    "uniform uint aref;\n"
    "uniform vec4 fog;\n"
    "void main() {\n"
    "    vec4 color = v_color;\n"
    "    if ((flags & 16u) != 0u) { color = vec4(floor(mix(fog.rgb, color.rgb, v_stqf.w)), color.a); }\n"
    "    if ((flags & 64u) != 0u) {\n"
    "        uint a = uint(color.a);\n"
    "        bool passed = atst == 1u || (atst == 2u && a < aref) || (atst == 3u && a <= aref) ||\n"
    "                      (atst == 4u && a == aref) || (atst == 5u && a >= aref) || (atst == 6u && a > aref) ||\n"
    "                      (atst == 7u && a != aref);\n"
    "        if (passed == ((flags & 128u) != 0u)) discard;\n"
    "    }\n"
    "    if ((flags & 512u) != 0u) { color = vec4(floor(color.rgb * color.a / 128.0), color.a); }\n"
    "    frag_color = vec4(color.rgb / 255.0, clamp(color.a / 255.0, 0.0, 1.0));\n"
    "}\n";

/* native_scanout (native.wgsl), the exact (non-"fast") form: da = display[0..3] (psm, bw, scale
   byte unused here, dbx), db = display[4..7] (dby, width, height, interlace), ds = display[8]
   (the resolution scale, set by this file as scanout() does in render.rs). */
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
    "    vec3 c = (int(xy.x) < size.x && int(xy.y) < size.y) ? texelFetch(source, ivec2(xy), 0).rgb : vec3(0.0);\n"
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

static GLuint compile(GLenum kind, const char *source)
{
    GLuint s = glCreateShader(kind);
    GLint ok = 0;
    glShaderSource(s, 1, &source, NULL);
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

/* A program from one or two shaders (compute: just cs); 0 on failure (logged). */
static GLuint link_program(const char *vs_src, const char *fs_src, const char *cs_src)
{
    GLuint vs = 0, fs = 0, cs = 0, program;
    if (cs_src) {
        if (!(cs = compile(GL_COMPUTE_SHADER, cs_src))) return 0;
    } else {
        if (!(vs = compile(GL_VERTEX_SHADER, vs_src)) || !(fs = compile(GL_FRAGMENT_SHADER, fs_src))) {
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

/* ---- creation ---- */

DhdrNative *dhdr_native_create(uint32_t flags)
{
    (void)flags;   /* DHDR_NATIVE_DUAL: dhdr_device_features never offers dual-source blending here */
    if (!gl_ready()) {
        fprintf(stderr, "gs native: no OpenGL context yet for the native renderer\n");
        return NULL;
    }
    DhdrNative *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    n->draw_program = link_program(native_vertex_source, native_fragment_source, NULL);
    n->scanout_program = link_program(NULL, NULL, scanout_source);
    n->present_program = link_program(present_vertex_source, present_fragment_source, NULL);
    if (!n->draw_program || !n->scanout_program || !n->present_program) {
        free(n);
        return NULL;
    }
    n->u_target_size = glGetUniformLocation(n->draw_program, "target_size");
    n->u_flags = glGetUniformLocation(n->draw_program, "flags");
    n->u_scale = glGetUniformLocation(n->draw_program, "nscale");
    n->u_atst = glGetUniformLocation(n->draw_program, "atst");
    n->u_aref = glGetUniformLocation(n->draw_program, "aref");
    n->u_fog = glGetUniformLocation(n->draw_program, "fog");
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
    fprintf(stderr, "gs native: GL native renderer ready (step 1: flat geometry, no textures yet)\n");
    return n;
}

void dhdr_native_destroy(DhdrNative *n)
{
    if (!n) return;
    if (gl_ready()) {
        for (int i = 0; i < DHDR_NATIVE_SURFACE_COUNT; i++) {
            if (n->surfaces[i].fbo) glDeleteFramebuffers(1, &n->surfaces[i].fbo);
            if (n->surfaces[i].texture) glDeleteTextures(1, &n->surfaces[i].texture);
        }
        if (n->display_fbo) glDeleteFramebuffers(1, &n->display_fbo);
        if (n->display_texture) glDeleteTextures(1, &n->display_texture);
        glDeleteBuffers(1, &n->vbo);
        glDeleteBuffers(1, &n->ibo);
        glDeleteVertexArrays(1, &n->vao);
        glDeleteProgram(n->draw_program);
        glDeleteProgram(n->scanout_program);
        glDeleteProgram(n->present_program);
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
    if (!n || index >= DHDR_NATIVE_SURFACE_COUNT || !gl_ready()) { size_out[0] = size_out[1] = 0; return 0; }
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

int dhdr_native_picture_indexed(DhdrNative *n, const DhdrPicture *p, const uint32_t *indices, uint32_t index_count,
                                DhdrPictureResult *out)
{
    memset(out, 0, sizeof *out);
    if (!n || !p || !gl_ready()) return 0;
    ensure_buffer(GL_ARRAY_BUFFER, n->vbo, &n->vbo_bytes, (size_t)p->vertex_count * sizeof(DhdrVertex));
    if (p->vertex_count) glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)((size_t)p->vertex_count * sizeof(DhdrVertex)),
                                        p->vertices);
    if (indices && index_count) {
        ensure_buffer(GL_ELEMENT_ARRAY_BUFFER, n->ibo, &n->ibo_bytes, (size_t)index_count * sizeof(uint32_t));
        glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, (GLsizeiptr)((size_t)index_count * sizeof(uint32_t)), indices);
    }
    glBindVertexArray(n->vao);
    glUseProgram(n->draw_program);
    glDisable(GL_DEPTH_TEST);   /* depth, blending: step 3 */
    glDisable(GL_BLEND);
    glEnable(GL_SCISSOR_TEST);
    GLint bound_fbo = -1;
    for (uint32_t i = 0; i < p->command_count; i++) {
        const DhdrCommand *k = &p->commands[i];
        if (k->kind != DHDR_CMD_DRAW || !k->count) continue;   /* COPY, IMPORT, ALPHA_UPLOAD: step 2 */
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
        glScissor(left, (int32_t)t->height - bottom, right - left, bottom - top);
        glColorMask((k->color_mask & 1) != 0, (k->color_mask & 2) != 0, (k->color_mask & 4) != 0,
                   (k->color_mask & 8) != 0);
        glUniform2f(n->u_target_size, (float)t->width / (float)target_scale, (float)t->height / (float)target_scale);
        glUniform1ui(n->u_flags, k->constants.flags);
        glUniform1ui(n->u_scale, target_scale);
        glUniform1ui(n->u_atst, k->constants.atst);
        glUniform1ui(n->u_aref, k->constants.aref);
        glUniform4f(n->u_fog, k->constants.fog[0], k->constants.fog[1], k->constants.fog[2], k->constants.fog[3]);
        if (indices && index_count) {
            glDrawElements(GL_TRIANGLES, (GLsizei)k->count, GL_UNSIGNED_INT,
                           (const void *)(uintptr_t)((size_t)k->first * sizeof(uint32_t)));
        } else {
            glDrawArrays(GL_TRIANGLES, (GLint)k->first, (GLsizei)k->count);
        }
        out->draws++;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    int shown = p->movie_on ? scan_out_movie(n, p->movie_width, p->movie_height, p->movie) : scan_out(n, p);
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
