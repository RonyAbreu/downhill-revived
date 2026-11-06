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
   texture on a dead list that dhdr_tex_collect drains on the render thread.

   Idle uploads (finding 145): a new texture also goes on a queue, holding a reference of its own,
   and the render thread uploads from it while it waits for the next field (dhdr_tex_idle_upload,
   called from native_core.inc's render loop), so the picture that samples it finds it done. Not a
   second thread: devkitPro's Mesa 20.1 nouveau gives every context of a screen the screen's one
   pushbuf (nvc0_create: nvc0->base.pushbuf = screen->base.pushbuf) with no lock, so a shared
   context on an upload thread would corrupt the command stream.

   The atlas (finding 145): what an upload cost on hardware was not the pixels but the driver's
   buffer objects -- each glTexStorage2D a new one, and each glTexSubImage2D a staging one made
   and freed again (nvc0_miptree_transfer_map), every one of them several IPC round trips to
   nvservices (nvMapCreate, nvAddressSpaceMap, UnmapBuffer, nvMapClose). So a decoded texture
   takes a slot of a page: one R32UI GL texture holding up to 64 textures of one size, made once
   and kept. Its texels are written by a draw that reads them from the upload ring through a
   texture buffer (one fixed buffer object, no staging), into the slot's rectangle of the page.
   The draws fetch at the slot's origin (dhdr_tex_origin). Replacements keep their own textures.

   DHD_SWITCH_UPLOAD picks the form: atlas (the default: the atlas, the ring and idle uploads),
   idle (idle uploads of a GL texture each, as before the atlas), render (the ring only) or direct
   (none of them: a GL texture each, filled by glTexSubImage2D from the CPU copy). */
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
    DhdrTexture *next_fresh;   /* the idle-upload queue (the queue holds a reference) */
    struct Page *page;         /* the atlas page it has a slot of (gl is the page's), or NULL */
    uint32_t slot, ox, oy;     /* the slot and its texel origin in the page */
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

/* The idle-upload queue, oldest first (the first sampled), under its own spin lock. g_fresh_count
   is read without the lock by the render thread's wait (dhdr_tex_idle_pending). */
static SDL_AtomicInt g_fresh_spin, g_fresh_count;
static DhdrTexture *g_fresh, *g_fresh_tail;
static void (*g_waker)(void *);
static void *g_waker_arg;

/* DHD_SWITCH_UPLOAD: 0 not read yet, then one of these. getenv, not SDL_getenv: dhd.ini reaches
   the environment after SDL took its copy. */
enum { MODE_ATLAS = 1, MODE_RENDER = 2, MODE_DIRECT = 3, MODE_IDLE = 4 };
static SDL_AtomicInt g_upload_mode;
static int upload_mode(void)
{
    int mode = SDL_GetAtomicInt(&g_upload_mode);
    if (!mode) {
        const char *s = getenv("DHD_SWITCH_UPLOAD");
        mode = s && !strcmp(s, "direct") ? MODE_DIRECT : s && !strcmp(s, "render") ? MODE_RENDER
             : s && !strcmp(s, "idle") ? MODE_IDLE : MODE_ATLAS;
        SDL_SetAtomicInt(&g_upload_mode, mode);
        fprintf(stderr, "gs native: texture uploads: %s (DHD_SWITCH_UPLOAD)\n",
                mode == MODE_ATLAS ? "atlas pages, written by the GPU from the ring, made while waiting for a field"
                : mode == MODE_IDLE ? "a GL texture each, made while waiting for a field"
                : mode == MODE_RENDER ? "a GL texture each through the ring, when first drawn"
                : "a GL texture each, from the CPU copy, when first drawn");
    }
    return mode;
}

static void fresh_push(DhdrTexture *t)
{
    SDL_AddAtomicInt(&t->refs, 1);   /* the queue's own */
    while (!SDL_CompareAndSwapAtomicInt(&g_fresh_spin, 0, 1)) { }
    t->next_fresh = NULL;
    if (g_fresh_tail) g_fresh_tail->next_fresh = t;
    else g_fresh = t;
    g_fresh_tail = t;
    int before = SDL_AddAtomicInt(&g_fresh_count, 1);
    SDL_SetAtomicInt(&g_fresh_spin, 0);
    /* The render thread may be asleep waiting for a field with the queue empty: wake it for the
       first texture. Its wait reads the count under the lock the waker takes, so this cannot fall
       between its check and its sleep. */
    if (!before && g_waker) g_waker(g_waker_arg);
}

static DhdrTexture *fresh_pop(void)
{
    while (!SDL_CompareAndSwapAtomicInt(&g_fresh_spin, 0, 1)) { }
    DhdrTexture *t = g_fresh;
    if (t) {
        g_fresh = t->next_fresh;
        if (!g_fresh) g_fresh_tail = NULL;
        SDL_AddAtomicInt(&g_fresh_count, -1);
    }
    SDL_SetAtomicInt(&g_fresh_spin, 0);
    return t;
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
    int mode = upload_mode();
    if ((mode == MODE_ATLAS || mode == MODE_IDLE) && __atomic_load_n(&g_waker, __ATOMIC_ACQUIRE))
        fresh_push(t);   /* no render loop to drain it yet: no queue */
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

/* What the uploads cost, for DHD_GL_TIMES and the slow field line (render_native.c prints them):
   how many, their time, their bytes; of that time, what making GL textures took, and how many
   uploads found a texture of their size in the pool instead. */
static uint64_t g_upload_count, g_upload_ns, g_upload_bytes, g_alloc_ns, g_reused;
/* The same for the uploads made while waiting for a field (dhdr_tex_idle_upload), kept apart. */
static uint64_t g_idle_count, g_idle_ns, g_idle_bytes;
static int g_in_idle;

void dhdr_tex_idle_stats(uint64_t *count, uint64_t *ns, uint64_t *bytes)
{
    *count = g_idle_count; *ns = g_idle_ns; *bytes = g_idle_bytes;
}

void dhdr_tex_upload_stats(uint64_t *count, uint64_t *ns, uint64_t *bytes)
{
    *count = g_upload_count; *ns = g_upload_ns; *bytes = g_upload_bytes;
}

void dhdr_tex_upload_split(uint64_t *alloc_ns, uint64_t *reused)
{
    *alloc_ns = g_alloc_ns; *reused = g_reused;
}

/* ---- the pool: GL textures of released DhdrTextures, kept for the next of the same shape ----

   Measured on hardware (finding 144): the first fields of a race uploaded 20-90 textures each,
   0.6-0.8 ms apiece for 20-40 KiB, which put those fields at 50-80 ms. Most of a decoded
   texture's life is short (the GS worker's cache turns over), and the sizes repeat (powers of
   two), so a released texture's GL object goes here instead of to glDeleteTextures, and the next
   upload of the same kind and size takes it: no new texture storage, only the pixels. The pool
   holds at most POOL_BYTES; past that the oldest go. Measured afterwards (finding 145): at a
   race's start every texture is new, so the pool reuses nothing there; the spikes it was meant
   for are still uploads, and only an upload thread with a shared context would move them. */
enum { POOL_MAX = 512 };
static const size_t POOL_BYTES = (size_t)64 << 20;
typedef struct Pooled { GLuint gl; uint32_t w, h, levels; int replacement; size_t bytes; } Pooled;
static Pooled g_pool[POOL_MAX];
static unsigned g_pool_count;
static size_t g_pool_bytes;

static size_t gl_bytes(const DhdrTexture *t)
{
    return t->replacement ? mip_bytes(t->w, t->h, t->levels) : (size_t)t->w * t->h * 4;
}

static GLuint pool_take(const DhdrTexture *t)
{
    for (unsigned i = g_pool_count; i-- > 0;) {   /* newest first */
        Pooled *e = &g_pool[i];
        if (e->w == t->w && e->h == t->h && e->levels == t->levels && e->replacement == t->replacement) {
            GLuint gl = e->gl;
            g_pool_bytes -= e->bytes;
            memmove(e, e + 1, (g_pool_count - i - 1) * sizeof *e);
            g_pool_count--;
            return gl;
        }
    }
    return 0;
}

static void pool_put(const DhdrTexture *t)
{
    size_t bytes = gl_bytes(t);
    if (bytes > POOL_BYTES / 4) { glDeleteTextures(1, &t->gl); return; }
    while (g_pool_count && (g_pool_count == POOL_MAX || g_pool_bytes + bytes > POOL_BYTES)) {
        glDeleteTextures(1, &g_pool[0].gl);   /* the oldest */
        g_pool_bytes -= g_pool[0].bytes;
        memmove(&g_pool[0], &g_pool[1], (g_pool_count - 1) * sizeof g_pool[0]);
        g_pool_count--;
    }
    g_pool[g_pool_count++] = (Pooled){ t->gl, t->w, t->h, t->levels, t->replacement, bytes };
    g_pool_bytes += bytes;
}

/* ---- the upload ring: the pixels go through one pixel unpack buffer ----

   glTexSubImage2D from client memory makes the driver copy the pixels into a staging buffer of
   its own each time. Here they are copied into one 8 MiB buffer instead, mapped unsynchronised,
   and the texture is filled from it on the GPU. The buffer is used as two halves: a half is
   written only after the fence set when it was last left has passed (a field or more ago, so
   that wait is normally free). An upload larger than a half goes the old way.
   DHD_SWITCH_UPLOAD = direct turns the ring off. */
enum { RING_BYTES = 8 << 20, RING_HALF = RING_BYTES / 2 };
static GLuint g_ring;
static GLsync g_ring_fence[2];
static size_t g_ring_at;      /* the next free byte, inside half g_ring_half */
static int g_ring_half;       /* the half being written (0 or 1) */
static int g_ring_off = -1;   /* -1 not decided yet */

/* Room for `bytes` in the ring: its offset, or (size_t)-1 to upload directly. */
static int ring_ready(void)
{
    if (g_ring_off < 0) g_ring_off = upload_mode() == MODE_DIRECT;
    if (g_ring_off) return 0;
    if (!g_ring) {
        glGenBuffers(1, &g_ring);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, g_ring);
        glBufferData(GL_PIXEL_UNPACK_BUFFER, RING_BYTES, NULL, GL_STREAM_DRAW);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        if (!g_ring) { g_ring_off = 1; return 0; }
    }
    return 1;
}

static size_t ring_reserve(size_t bytes)
{
    if (bytes > RING_HALF || !ring_ready()) return (size_t)-1;
    /* The half is kept, not worked out from g_ring_at: rounded up to 256, g_ring_at can land
       exactly on the end of the buffer, and a half computed from it would be 2 -- past both the
       buffer and g_ring_fence, which is how the first version of this wrote over the pool's
       count and crashed (finding 144). */
    int half = g_ring_half;
    size_t end = (size_t)(half + 1) * RING_HALF;
    if (g_ring_at + bytes > end) {   /* this half is full: fence it, move to the other */
        if (g_ring_fence[half]) glDeleteSync(g_ring_fence[half]);
        g_ring_fence[half] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        half ^= 1;
        g_ring_half = half;
        g_ring_at = (size_t)half * RING_HALF;
        if (g_ring_fence[half]) {
            glClientWaitSync(g_ring_fence[half], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
            glDeleteSync(g_ring_fence[half]);
            g_ring_fence[half] = 0;
        }
    }
    size_t at = g_ring_at;
    g_ring_at = (at + bytes + 255) & ~(size_t)255;
    return at;
}

/* `bytes` of src into the ring: its offset there, or (size_t)-1 (no room, or no mapping). The
   ring is left bound to GL_PIXEL_UNPACK_BUFFER when it worked. */
static size_t ring_write(const void *src, size_t bytes)
{
    size_t at = ring_reserve(bytes);
    if (at == (size_t)-1) return at;
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, g_ring);
    void *map = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, (GLintptr)at, (GLsizeiptr)bytes,
                                 GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
    if (!map) { glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0); return (size_t)-1; }
    memcpy(map, src, bytes);
    glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
    return at;
}

/* ---- the atlas: decoded textures as slots of shared R32UI pages ---- */

typedef struct Page {
    GLuint gl;
    uint32_t w, h, cols, rows;   /* the slot size, and the slots across and down (cols * rows <= 64) */
    uint64_t used;               /* a bit per slot */
    unsigned count;
    struct Page *next;
} Page;
static Page *g_pages;
static uint64_t g_page_count, g_page_bytes;
static GLuint g_copy_attached;   /* the page the GPU copy's framebuffer has attached (below) */

static unsigned page_slots(const Page *p) { return p->cols * p->rows; }

/* A free slot of w x h: a page's index in *slot, a new page made when every one is full (its
   time counted as allocation). NULL when GL would not make one. */
static Page *atlas_take(uint32_t w, uint32_t h, uint32_t *slot)
{
    for (Page *p = g_pages; p; p = p->next)
        if (p->w == w && p->h == h && p->count < page_slots(p)) {
            for (uint32_t i = 0; i < page_slots(p); i++)
                if (!(p->used >> i & 1)) { p->used |= 1ull << i; p->count++; *slot = i; g_reused++; return p; }
        }
    uint64_t began = SDL_GetTicksNS();
    Page *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->w = w; p->h = h;
    /* Up to 8 x 8 slots, a page at most 1024 texels a side (a texture of 1024 has a page alone). */
    p->cols = w >= 1024 ? 1 : 1024 / w < 8 ? 1024 / w : 8;
    p->rows = h >= 1024 ? 1 : 1024 / h < 8 ? 1024 / h : 8;
    glGenTextures(1, &p->gl);
    if (!p->gl) { free(p); return NULL; }
    glBindTexture(GL_TEXTURE_2D, p->gl);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, (GLsizei)(p->cols * w), (GLsizei)(p->rows * h));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    p->next = g_pages;
    g_pages = p;
    g_page_count++;
    g_page_bytes += (uint64_t)p->cols * w * p->rows * h * 4;
    p->used = 1; p->count = 1; *slot = 0;
    g_alloc_ns += SDL_GetTicksNS() - began;
    return p;
}

static void page_delete(Page *gone)
{
    for (Page **at = &g_pages; *at; at = &(*at)->next)
        if (*at == gone) { *at = gone->next; break; }
    /* The copy's framebuffer keeps a deleted texture attached, and its name can come back for a
       new page: attach afresh next time. */
    if (g_copy_attached == gone->gl) g_copy_attached = 0;
    glDeleteTextures(1, &gone->gl);
    g_page_count--;
    g_page_bytes -= (uint64_t)gone->cols * gone->w * gone->rows * gone->h * 4;
    free(gone);
}

/* A released texture's slot back. An empty page is kept for the next of its size unless another
   empty one of that size is already kept. */
static void atlas_give(DhdrTexture *t)
{
    Page *p = t->page;
    p->used &= ~(1ull << t->slot);
    p->count--;
    if (p->count) return;
    for (Page *q = g_pages; q; q = q->next)
        if (q != p && !q->count && q->w == p->w && q->h == p->h) { page_delete(p); return; }
}

/* ---- the GPU copy: ring texels into a page's rectangle, by a draw ---- */

#define COPY_UNIT 15   /* a texture unit the renderer never uses */
static GLuint g_copy_program, g_copy_vao, g_copy_fbo, g_copy_tbo;
static GLint g_copy_origin, g_copy_width, g_copy_base;
static int g_copy_ok = -1;   /* -1 not tried yet */

static const char *copy_vertex_source =
    "#version 430 core\n"
    "void main() {\n"   /* one triangle over the whole viewport */
    "    vec2 p = vec2(float((gl_VertexID & 1) * 4 - 1), float((gl_VertexID >> 1) * 4 - 1));\n"
    "    gl_Position = vec4(p, 0.0, 1.0);\n"
    "}\n";
static const char *copy_fragment_source =
    "#version 430 core\n"
    "uniform usamplerBuffer ring;\n"
    "uniform ivec2 origin;\n"   /* the slot's texel origin in the page (the viewport's) */
    "uniform int width;\n"
    "uniform int base;\n"       /* the texture's first texel in the ring */
    "out uvec4 texel;\n"
    "void main() {\n"
    "    ivec2 at = ivec2(gl_FragCoord.xy) - origin;\n"
    "    texel = uvec4(texelFetch(ring, base + at.y * width + at.x).x, 0u, 0u, 0u);\n"
    "}\n";

static GLuint copy_shader(GLenum kind, const char *source)
{
    GLuint sh = glCreateShader(kind);
    GLint ok = 0;
    glShaderSource(sh, 1, &source, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        fprintf(stderr, "gs native: the atlas copy shader: %s\n", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static int copy_ready(void)
{
    if (g_copy_ok >= 0) return g_copy_ok;
    g_copy_ok = 0;
    GLint most = 0;
    glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &most);
    if (most < RING_BYTES / 4 || !ring_ready()) {
        fprintf(stderr, "gs native: no atlas copy by the GPU (texture buffers of %d texels)\n", most);
        return 0;
    }
    GLuint vs = copy_shader(GL_VERTEX_SHADER, copy_vertex_source), fs = copy_shader(GL_FRAGMENT_SHADER, copy_fragment_source);
    if (!vs || !fs) return 0;
    g_copy_program = glCreateProgram();
    glAttachShader(g_copy_program, vs);
    glAttachShader(g_copy_program, fs);
    glLinkProgram(g_copy_program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = 0;
    glGetProgramiv(g_copy_program, GL_LINK_STATUS, &linked);
    if (!linked) { fprintf(stderr, "gs native: the atlas copy program did not link\n"); return 0; }
    g_copy_origin = glGetUniformLocation(g_copy_program, "origin");
    g_copy_width = glGetUniformLocation(g_copy_program, "width");
    g_copy_base = glGetUniformLocation(g_copy_program, "base");
    GLint program = 0, active = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glUseProgram(g_copy_program);
    glUniform1i(glGetUniformLocation(g_copy_program, "ring"), COPY_UNIT);
    glUseProgram((GLuint)program);
    glGenVertexArrays(1, &g_copy_vao);
    glGenFramebuffers(1, &g_copy_fbo);
    glGenTextures(1, &g_copy_tbo);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glActiveTexture(GL_TEXTURE0 + COPY_UNIT);
    glBindTexture(GL_TEXTURE_BUFFER, g_copy_tbo);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, g_ring);
    glActiveTexture((GLenum)active);
    g_copy_ok = g_copy_vao && g_copy_fbo && g_copy_tbo;
    return g_copy_ok;
}

/* The renderer's GL state the copy changes, put back after it: the copy runs inside a picture
   (the first draw of a texture not uploaded yet) as well as between pictures. */
typedef struct {
    GLint fbo, program, vao, active, viewport[4];
    GLboolean blend, depth, scissor, stencil, cull, discard, mask[4];
} Saved;

static void state_save(Saved *s)
{
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s->fbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &s->program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s->vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s->active);
    glGetIntegerv(GL_VIEWPORT, s->viewport);
    s->blend = glIsEnabled(GL_BLEND); s->depth = glIsEnabled(GL_DEPTH_TEST);
    s->scissor = glIsEnabled(GL_SCISSOR_TEST); s->stencil = glIsEnabled(GL_STENCIL_TEST);
    s->cull = glIsEnabled(GL_CULL_FACE); s->discard = glIsEnabled(GL_RASTERIZER_DISCARD);
    glGetBooleani_v(GL_COLOR_WRITEMASK, 0, s->mask);
}

static void enable(GLenum cap, GLboolean on) { if (on) glEnable(cap); else glDisable(cap); }

static void state_restore(const Saved *s)
{
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)s->fbo);
    glUseProgram((GLuint)s->program);
    glBindVertexArray((GLuint)s->vao);
    glActiveTexture((GLenum)s->active);
    glViewport(s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3]);
    enable(GL_BLEND, s->blend); enable(GL_DEPTH_TEST, s->depth); enable(GL_SCISSOR_TEST, s->scissor);
    enable(GL_STENCIL_TEST, s->stencil); enable(GL_CULL_FACE, s->cull); enable(GL_RASTERIZER_DISCARD, s->discard);
    glColorMaski(0, s->mask[0], s->mask[1], s->mask[2], s->mask[3]);
}

/* t's texels from ring offset `at` into its slot. */
static void copy_draw(const DhdrTexture *t, size_t at)
{
    Saved s;
    state_save(&s);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_copy_fbo);
    if (g_copy_attached != t->page->gl) {
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->page->gl, 0);
        g_copy_attached = t->page->gl;
    }
    glViewport((GLint)t->ox, (GLint)t->oy, (GLsizei)t->w, (GLsizei)t->h);
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE); glDisable(GL_RASTERIZER_DISCARD);
    glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(g_copy_program);
    glUniform2i(g_copy_origin, (GLint)t->ox, (GLint)t->oy);
    glUniform1i(g_copy_width, (GLint)t->w);
    glUniform1i(g_copy_base, (GLint)(at / 4));
    glActiveTexture(GL_TEXTURE0 + COPY_UNIT);
    glBindTexture(GL_TEXTURE_BUFFER, g_copy_tbo);
    glBindVertexArray(g_copy_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    state_restore(&s);
}

/* A decoded texture into an atlas slot: by the GPU from the ring, else by glTexSubImage2D. */
static int atlas_upload(DhdrTexture *t)
{
    uint32_t slot;
    Page *p = atlas_take(t->w, t->h, &slot);
    if (!p) return 0;
    t->page = p; t->slot = slot;
    t->ox = slot % p->cols * t->w; t->oy = slot / p->cols * t->h;
    t->gl = p->gl;
    size_t at = copy_ready() ? ring_write(t->pixels, t->bytes) : (size_t)-1;
    if (at != (size_t)-1) {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        copy_draw(t, at);
    } else {
        glBindTexture(GL_TEXTURE_2D, p->gl);
        glTexSubImage2D(GL_TEXTURE_2D, 0, (GLint)t->ox, (GLint)t->oy, (GLsizei)t->w, (GLsizei)t->h,
                        GL_RED_INTEGER, GL_UNSIGNED_INT, t->pixels);
    }
    return 1;
}

static void upload(DhdrTexture *t)
{
    uint64_t began = SDL_GetTicksNS();
    size_t bytes = t->bytes;
    uint64_t *count = g_in_idle ? &g_idle_count : &g_upload_count, *spent = g_in_idle ? &g_idle_ns : &g_upload_ns;
    uint64_t *moved = g_in_idle ? &g_idle_bytes : &g_upload_bytes;
    if (!t->replacement && upload_mode() == MODE_ATLAS) {
        if (!atlas_upload(t)) { t->failed = 1; return; }
        goto uploaded;
    }
    GLuint gl = pool_take(t);
    int fresh = !gl;
    if (fresh) glGenTextures(1, &gl);
    if (!gl) { t->failed = 1; return; }
    t->gl = gl;
    glBindTexture(GL_TEXTURE_2D, t->gl);
    if (fresh) {
        if (t->replacement) {
            glTexStorage2D(GL_TEXTURE_2D, (GLsizei)t->levels, GL_RGBA8, (GLsizei)t->w, (GLsizei)t->h);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)t->levels - 1);
        } else {
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, (GLsizei)t->w, (GLsizei)t->h);
            /* An integer texture is never filtered; the shader fetches texels itself (texel_u). */
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }
        g_alloc_ns += SDL_GetTicksNS() - began;
    } else g_reused++;
    const uint8_t *source = t->pixels;
    size_t at = ring_write(t->pixels, bytes);
    if (at != (size_t)-1) source = (const uint8_t *)(uintptr_t)at;   /* an offset into the bound ring */
    if (t->replacement) {
        const uint8_t *level_at = source;
        uint32_t lw = t->w, lh = t->h;
        for (uint32_t level = 0; level < t->levels; level++) {
            glTexSubImage2D(GL_TEXTURE_2D, (GLint)level, 0, 0, (GLsizei)lw, (GLsizei)lh,
                            GL_RGBA, GL_UNSIGNED_BYTE, level_at);
            level_at += (size_t)lw * lh * 4;
            lw = lw > 1 ? lw / 2 : 1;
            lh = lh > 1 ? lh / 2 : 1;
        }
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)t->w, (GLsizei)t->h,
                        GL_RED_INTEGER, GL_UNSIGNED_INT, source);
    }
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
uploaded:
    /* The pixels are in GL now; on a 4 GiB console the CPU copy is worth giving back. */
    free(t->pixels);
    t->pixels = NULL;
    t->bytes = 0;
    (*count)++;
    *moved += bytes;
    *spent += SDL_GetTicksNS() - began;
}

GLuint dhdr_tex_gl(DhdrTexture *t)
{
    if (!t || t->failed) return 0;
    if (!t->gl) upload(t);
    return t->gl;
}

void dhdr_tex_idle_waker(void (*wake)(void *), void *arg)
{
    g_waker_arg = arg;
    __atomic_store_n(&g_waker, wake, __ATOMIC_RELEASE);
}

int dhdr_tex_idle_pending(void)
{
    return SDL_GetAtomicInt(&g_fresh_count) > 0;
}

int dhdr_tex_idle_upload(void)
{
    DhdrTexture *t = fresh_pop();
    if (!t) return 0;
    /* Uploaded already (a picture sampled it first) or released by everyone but the queue (it
       would only go to the pool): nothing to do but let the queue's reference go. */
    if (!t->gl && !t->failed && SDL_GetAtomicInt(&t->refs) > 1) {
        g_in_idle = 1;
        upload(t);
        g_in_idle = 0;
    }
    dhdr_texture_release(t);
    return 1;
}

int dhdr_tex_is_replacement(const DhdrTexture *t) { return t && t->replacement; }

void dhdr_tex_origin(const DhdrTexture *t, int32_t *x, int32_t *y)
{
    *x = t && t->page ? (int32_t)t->ox : 0;
    *y = t && t->page ? (int32_t)t->oy : 0;
}

void dhdr_tex_atlas_stats(uint64_t *pages, uint64_t *bytes)
{
    *pages = g_page_count; *bytes = g_page_bytes;
}

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
        if (dead->page) atlas_give(dead);
        else if (dead->gl) pool_put(dead);
        free(dead->pixels);
        free(dead);
        dead = next;
    }
}

void dhdr_tex_shutdown(void)
{
    __atomic_store_n(&g_waker, NULL, __ATOMIC_RELEASE);
    for (DhdrTexture *t; (t = fresh_pop());) dhdr_texture_release(t);
    dhdr_tex_collect();
    for (unsigned i = 0; i < g_pool_count; i++) glDeleteTextures(1, &g_pool[i].gl);
    while (g_pages) page_delete(g_pages);
    if (g_copy_program) glDeleteProgram(g_copy_program);
    if (g_copy_vao) glDeleteVertexArrays(1, &g_copy_vao);
    if (g_copy_fbo) glDeleteFramebuffers(1, &g_copy_fbo);
    if (g_copy_tbo) glDeleteTextures(1, &g_copy_tbo);
    g_copy_program = g_copy_vao = g_copy_fbo = g_copy_tbo = g_copy_attached = 0;
    g_copy_ok = -1;
    g_pool_count = 0; g_pool_bytes = 0;
    for (int i = 0; i < 2; i++)
        if (g_ring_fence[i]) { glDeleteSync(g_ring_fence[i]); g_ring_fence[i] = 0; }
    if (g_ring) glDeleteBuffers(1, &g_ring);
    g_ring = 0; g_ring_at = 0; g_ring_half = 0;
    for (unsigned i = 0; i < 8; i++) {
        if (g_samplers[i]) glDeleteSamplers(1, &g_samplers[i]);
        g_samplers[i] = 0;
    }
    if (g_dummy_texels) glDeleteTextures(1, &g_dummy_texels);
    if (g_dummy_rgba) glDeleteTextures(1, &g_dummy_rgba);
    g_dummy_texels = g_dummy_rgba = 0;
}
