/* The Switch's DhdrTexture (NX2 step 2, finding 144): decoded textures and replacements, the C
   stand-in for port/render/src/native/textures.rs.

   A decoded texture is w x h R32UI, each texel its RGBA8 bytes read as one integer (finding 131,
   as the Rust renderer's R32Uint), fetched by native.wgsl's `_u` path. A replacement (a texture
   pack's or the sharp font's) is RGBA8 with `levels` mip levels, each level's rows after the
   previous level's (sides halved, down to 1), sampled through one of the eight samplers
   shared.rs makes (bilinear | U clamped << 1 | V clamped << 2).

   Threads (textures_gl.h): create, retain and release run anywhere, so they only touch the CPU
   copy and an atomic reference count; the GL texture is built the first time the render thread
   samples it (dhdr_tex_gl), and the CPU copy is freed then. Releasing the last reference puts the
   texture on a dead list that dhdr_tex_collect drains on the render thread. */
#include <glad/glad.h>
#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dhd_render.h"
#include "dhd_render_ext.h"   /* dhdr_device_max_texture */
#include "textures_gl.h"

/* GL_EXT_texture_filter_anisotropic's names, core as these since 4.6; glad may declare only the
   EXT ones, or neither (the query below then simply fails). */
#ifndef GL_TEXTURE_MAX_ANISOTROPY
#define GL_TEXTURE_MAX_ANISOTROPY 0x84FE
#endif
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY
#define GL_MAX_TEXTURE_MAX_ANISOTROPY 0x84FF
#endif

struct DhdrTexture {
    SDL_AtomicInt refs;
    uint32_t w, h, levels;
    int replacement;           /* RGBA8 mips (sampled) rather than R32UI texels (fetched) */
    uint8_t *pixels;           /* the CPU copy until it is uploaded, then NULL */
    size_t bytes;
    GLuint gl;                 /* 0 until the render thread uploads it */
    int failed;                /* the upload failed once; never tried again */
    DhdrTexture *next_dead;
};

/* The dead list: textures whose last reference went, waiting for the render thread. A spin lock
   rather than an SDL_Mutex, so there is nothing to create before the first texture (a mutex made
   lazily would need its own lock) and nothing to destroy at the end; the list is touched for a
   handful of instructions, from at most a few threads. */
static SDL_AtomicInt g_dead_spin;
static DhdrTexture *g_dead;

/* Render-thread-only objects, made on first use. */
static GLuint g_samplers[8];
static GLuint g_dummy_texels, g_dummy_rgba;

static void dead_lock(void)
{
    while (!SDL_CompareAndSwapAtomicInt(&g_dead_spin, 0, 1)) { /* contended for a few instructions */ }
}

static void dead_unlock(void)
{
    SDL_SetAtomicInt(&g_dead_spin, 0);
}

/* The bytes of `levels` RGBA8 mip levels of a w x h texture (native::mip_bytes), 0 when the
   levels do not fit the size. */
static size_t mip_bytes(uint32_t w, uint32_t h, uint32_t levels)
{
    if (!w || !h || !levels || levels > 32) return 0;
    size_t total = 0;
    uint32_t lw = w, lh = h;
    for (uint32_t i = 0; i < levels; i++) {
        total += (size_t)lw * lh * 4;
        if (lw == 1 && lh == 1 && i + 1 < levels) return 0;   /* more levels than the size allows */
        lw = lw > 1 ? lw / 2 : 1;
        lh = lh > 1 ? lh / 2 : 1;
    }
    return total;
}

static int size_ok(uint32_t w, uint32_t h)
{
    uint32_t max = dhdr_device_max_texture();
    if (!max) max = 4096;
    if (!w || !h || w > max || h > max) {
        fprintf(stderr, "gs native: texture size %u x %u (1 to %u allowed)\n", w, h, max);
        return 0;
    }
    return 1;
}

static DhdrTexture *make(const void *src, size_t bytes, uint32_t w, uint32_t h, uint32_t levels, int replacement)
{
    DhdrTexture *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->pixels = malloc(bytes);
    if (!t->pixels) { free(t); return NULL; }
    memcpy(t->pixels, src, bytes);
    t->bytes = bytes;
    t->w = w; t->h = h; t->levels = levels;
    t->replacement = replacement;
    SDL_SetAtomicInt(&t->refs, 1);
    return t;
}

DhdrTexture *dhdr_texture_create(const uint32_t *rgba, uint32_t w, uint32_t h)
{
    if (!rgba || !size_ok(w, h)) return NULL;
    return make(rgba, (size_t)w * h * 4, w, h, 1, 0);
}

DhdrTexture *dhdr_replace_create(const uint8_t *rgba_mips, uint32_t w, uint32_t h, uint32_t levels)
{
    size_t bytes = mip_bytes(w, h, levels);
    if (!rgba_mips || !bytes || !size_ok(w, h)) {
        if (rgba_mips && !bytes) fprintf(stderr, "gs native: replacement %u x %u with %u levels\n", w, h, levels);
        return NULL;
    }
    return make(rgba_mips, bytes, w, h, levels, 1);
}

void dhdr_texture_retain(DhdrTexture *t)
{
    if (t) SDL_AddAtomicInt(&t->refs, 1);
}

void dhdr_texture_release(DhdrTexture *t)
{
    if (!t) return;
    if (SDL_AddAtomicInt(&t->refs, -1) != 1) return;   /* the value before the add */
    /* The last reference: nobody can reach t any more. Its GL texture (if it has one) belongs to
       the render thread, so hand the whole texture over instead of freeing it here. */
    dead_lock();
    t->next_dead = g_dead;
    g_dead = t;
    dead_unlock();
}

/* ---- the render thread ---- */

/* What the uploads cost, for DHD_GL_TIMES (render_native.c prints them). */
static uint64_t g_upload_count, g_upload_ns, g_upload_bytes;

void dhdr_tex_upload_stats(uint64_t *count, uint64_t *ns, uint64_t *bytes)
{
    *count = g_upload_count; *ns = g_upload_ns; *bytes = g_upload_bytes;
}

static void upload(DhdrTexture *t)
{
    uint64_t began = SDL_GetTicksNS();
    size_t bytes = t->bytes;
    glGenTextures(1, &t->gl);
    if (!t->gl) { t->failed = 1; return; }
    glBindTexture(GL_TEXTURE_2D, t->gl);
    if (t->replacement) {
        glTexStorage2D(GL_TEXTURE_2D, (GLsizei)t->levels, GL_RGBA8, (GLsizei)t->w, (GLsizei)t->h);
        const uint8_t *at = t->pixels;
        uint32_t lw = t->w, lh = t->h;
        for (uint32_t level = 0; level < t->levels; level++) {
            glTexSubImage2D(GL_TEXTURE_2D, (GLint)level, 0, 0, (GLsizei)lw, (GLsizei)lh,
                            GL_RGBA, GL_UNSIGNED_BYTE, at);
            at += (size_t)lw * lh * 4;
            lw = lw > 1 ? lw / 2 : 1;
            lh = lh > 1 ? lh / 2 : 1;
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)t->levels - 1);
    } else {
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, (GLsizei)t->w, (GLsizei)t->h);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)t->w, (GLsizei)t->h,
                        GL_RED_INTEGER, GL_UNSIGNED_INT, t->pixels);
        /* An integer texture is never filtered; the shader fetches texels itself (texel_u). */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    /* The pixels are in GL now; on a 4 GiB console the CPU copy is worth giving back. */
    free(t->pixels);
    t->pixels = NULL;
    t->bytes = 0;
    g_upload_count++;
    g_upload_bytes += bytes;
    g_upload_ns += SDL_GetTicksNS() - began;
}

GLuint dhdr_tex_gl(DhdrTexture *t)
{
    if (!t || t->failed) return 0;
    if (!t->gl) upload(t);
    return t->gl;
}

int dhdr_tex_is_replacement(const DhdrTexture *t) { return t && t->replacement; }

void dhdr_tex_size(const DhdrTexture *t, uint32_t *w, uint32_t *h, uint32_t *levels_out)
{
    if (w) *w = t ? t->w : 0;
    if (h) *h = t ? t->h : 0;
    if (levels_out) *levels_out = t ? t->levels : 0;
}

GLuint dhdr_tex_sampler(unsigned index)
{
    index &= 7;
    if (!g_samplers[index]) {
        GLuint s = 0;
        glGenSamplers(1, &s);
        if (!s) return 0;
        int bilinear = (index & 1) != 0;
        /* shared.rs: min and mipmap always linear, mag linear only when the draw is bilinear,
           U and V clamped by bits 1 and 2, W clamped (no 3D sampling here). */
        glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, bilinear ? GL_LINEAR : GL_NEAREST);
        glSamplerParameteri(s, GL_TEXTURE_WRAP_S, (index & 2) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        glSamplerParameteri(s, GL_TEXTURE_WRAP_T, (index & 4) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        glSamplerParameteri(s, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glSamplerParameterf(s, GL_TEXTURE_MIN_LOD, 0.0f);
        glSamplerParameterf(s, GL_TEXTURE_MAX_LOD, 32.0f);
        /* wgpu asks the bilinear samplers for 16x anisotropy. GL_EXT_texture_filter_anisotropic
           is core only since 4.6, and whether devkitPro's glad declares its flag depends on how
           that glad was generated, so ask the driver itself: the query sets an error and leaves
           `most` alone when the extension is missing. */
        if (bilinear) {
            GLfloat most = 1.0f;
            while (glGetError() != GL_NO_ERROR) { }
            glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &most);
            if (glGetError() == GL_NO_ERROR && most > 1.0f) {
                glSamplerParameterf(s, GL_TEXTURE_MAX_ANISOTROPY, most > 16.0f ? 16.0f : most);
                while (glGetError() != GL_NO_ERROR) { }
            }
        }
        g_samplers[index] = s;
    }
    return g_samplers[index];
}

static GLuint dummy(GLuint *slot, GLenum format, GLenum layout, GLenum type, const void *texel)
{
    if (!*slot) {
        glGenTextures(1, slot);
        if (!*slot) return 0;
        glBindTexture(GL_TEXTURE_2D, *slot);
        glTexStorage2D(GL_TEXTURE_2D, 1, format, 1, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, layout, type, texel);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    return *slot;
}

GLuint dhdr_tex_dummy_texels(void)
{
    static const uint32_t zero = 0;
    return dummy(&g_dummy_texels, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
}

GLuint dhdr_tex_dummy_rgba(void)
{
    static const uint8_t zero[4] = { 0, 0, 0, 0 };
    return dummy(&g_dummy_rgba, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, zero);
}

void dhdr_tex_collect(void)
{
    DhdrTexture *dead;
    dead_lock();
    dead = g_dead;
    g_dead = NULL;
    dead_unlock();
    while (dead) {
        DhdrTexture *next = dead->next_dead;
        if (dead->gl) glDeleteTextures(1, &dead->gl);
        free(dead->pixels);
        free(dead);
        dead = next;
    }
}

void dhdr_tex_shutdown(void)
{
    dhdr_tex_collect();
    for (unsigned i = 0; i < 8; i++) {
        if (g_samplers[i]) glDeleteSamplers(1, &g_samplers[i]);
        g_samplers[i] = 0;
    }
    if (g_dummy_texels) glDeleteTextures(1, &g_dummy_texels);
    if (g_dummy_rgba) glDeleteTextures(1, &g_dummy_rgba);
    g_dummy_texels = g_dummy_rgba = 0;
}
