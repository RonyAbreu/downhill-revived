/* The Switch's renderer, milestone NX2 step 1 (finding 144): presentation on OpenGL 4.3 core
   (Mesa's nouveau on the Tegra X1, through SDL's EGL window and devkitPro's glad). The Rust
   renderer (port/render, wgpu) cannot run on Horizon; this file stands in for its device, surface
   and present entries of dhd_render.h.

   What step 1 shows: pictures handed over as pixels (dhdr_present_exact with pixels: the movies,
   decoded on the CPU), drawn 4:3 in the middle of the screen. The exact renderer is accepted but
   draws nothing (dhdr_exact_* below: its reads leave the caller's bytes as they are, it has no
   display texture), so between movies the screen stays black; the native renderer is still
   render_stubs.c's refusal. Steps 2 and 3 draw the GS.

   Threads: present.c makes the window on the main thread (switch_gl_attributes first) and presents
   from whichever thread runs gs_vsync; the OpenGL context is made on the first present, current on
   the thread presenting (made current again if another thread takes over: one at a time, as
   dhd_render.h's surface rule says). */
#include <glad/glad.h>
#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dhd_render.h"
#include "dhd_render_ext.h"
#include "render_gl_ext.h"

struct DhdrSurface {
    SDL_Window *window;
    SDL_GLContext gl;
    SDL_ThreadID thread;          /* the thread the context is current on */
    int interval;                 /* swap interval: 1 waits for the display (V-Sync), 0 not */
    int failed;
    GLuint program, vao, texture;
    GLint rect;                   /* the program's rect uniform */
    uint32_t texture_w, texture_h;
};

struct DhdrExact { int unused; };

static char adapter[160] = "";
/* render_native.c's own GL calls (building surfaces, drawing a picture) run before any
   dhdr_native_present call: this single-window game has one surface, so that is the one to
   make current (render_gl_ext.h). */
static DhdrSurface *g_active_surface;

/* present.c, after SDL's video starts and before the window: the window's EGL configuration. */
void switch_gl_attributes(void)
{
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
}

/* ---- device ---- */

int dhdr_device_init(const DhdrDeviceDesc *d)
{
    (void)d;
    snprintf(adapter, sizeof adapter, "Tegra X1 (OpenGL, not started yet)");
    return 1;
}
const char *dhdr_adapter_name(void) { return adapter; }
uint32_t dhdr_device_features(void) { return 0; }
const char *dhdr_version(void) { return "dhd_render switch-gl 0.1 (NX2 step 1: presentation)"; }
uint32_t dhdr_device_max_texture(void) { return 4096; }
uint32_t dhdr_perf_read(uint64_t *out, uint32_t count) { (void)out; (void)count; return 0; }

/* ---- the exact renderer: accepted, draws nothing (step 1) ---- */

DhdrExact *dhdr_exact_create(uint32_t raster_flags)
{
    (void)raster_flags;
    static DhdrExact none;
    fprintf(stderr, "gs: the Switch has no exact renderer yet: only movies are shown\n");
    return &none;
}
void dhdr_exact_destroy(DhdrExact *x) { (void)x; }
void dhdr_exact_write(DhdrExact *x, uint32_t buffer, uint32_t offset, const void *data, uint32_t bytes)
{ (void)x; (void)buffer; (void)offset; (void)data; (void)bytes; }
void dhdr_exact_copy(DhdrExact *x, uint32_t from, uint32_t from_offset, uint32_t to, uint32_t to_offset, uint32_t bytes)
{ (void)x; (void)from; (void)from_offset; (void)to; (void)to_offset; (void)bytes; }
void dhdr_exact_batch(DhdrExact *x, uint32_t dynamic_offset, uint32_t groups_x, uint32_t groups_y)
{ (void)x; (void)dynamic_offset; (void)groups_x; (void)groups_y; }
void dhdr_exact_record(DhdrExact *x, uint32_t pipeline, uint32_t x_word, uint32_t y_word)
{ (void)x; (void)pipeline; (void)x_word; (void)y_word; }
void dhdr_exact_submit(DhdrExact *x) { (void)x; }
/* A read succeeds and leaves image as it is (gpu_rust.c's op_read ends the program on a failure). */
int dhdr_exact_read_begin(DhdrExact *x, uint32_t buffer, const uint32_t want[16]) { (void)x; (void)buffer; (void)want; return 1; }
int dhdr_exact_read_finish(DhdrExact *x, uint32_t buffer, const uint32_t want[16], void *image)
{ (void)x; (void)buffer; (void)want; (void)image; return 1; }
int dhdr_exact_capture_begin(DhdrExact *x, uint32_t *w, uint32_t *h) { (void)x; *w = *h = 0; return 0; }
int dhdr_exact_capture_finish(DhdrExact *x, uint32_t *pixels) { (void)x; (void)pixels; return 0; }
void dhdr_exact_stats(const DhdrExact *x, DhdrExactStats *out) { (void)x; memset(out, 0, sizeof *out); }

/* ---- surface and presentation ---- */

static const char *vertex_source =
    "#version 430 core\n"
    "uniform vec4 rect;\n"          /* left, top, right, bottom in clip space */
    "out vec2 uv;\n"
    "void main() {\n"
    "    vec2 corner = vec2(gl_VertexID & 1, gl_VertexID >> 1);\n"
    "    uv = corner;\n"                /* the picture's first row at the top */
    "    gl_Position = vec4(mix(rect.xy, rect.zw, corner), 0.0, 1.0);\n"
    "}\n";
static const char *fragment_source =
    "#version 430 core\n"
    "uniform sampler2D picture;\n"
    "in vec2 uv;\n"
    "out vec4 color;\n"
    "void main() { color = vec4(texture(picture, uv).rgb, 1.0); }\n";

static GLuint shader(GLenum kind, const char *source)
{
    GLuint s = glCreateShader(kind);
    GLint ok = 0;
    glShaderSource(s, 1, &source, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "gs: OpenGL shader: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

/* The context, current on this thread; made on the first call. 0: no OpenGL (logged once). */
static int current(DhdrSurface *s)
{
    if (s->failed) return 0;
    SDL_ThreadID me = SDL_GetCurrentThreadID();
    if (s->gl) {
        if (s->thread != me) {
            if (!SDL_GL_MakeCurrent(s->window, s->gl)) {
                fprintf(stderr, "gs: OpenGL context on another thread (%s)\n", SDL_GetError());
                return 0;
            }
            s->thread = me;
        }
        return 1;
    }
    s->failed = 1;
    if (!(s->gl = SDL_GL_CreateContext(s->window))) {
        fprintf(stderr, "gs: no OpenGL 4.3 context (%s)\n", SDL_GetError());
        return 0;
    }
    s->thread = me;
    if (!gladLoadGL()) {
        fprintf(stderr, "gs: OpenGL functions not loaded (glad)\n");
        return 0;
    }
    SDL_GL_SetSwapInterval(s->interval);
    snprintf(adapter, sizeof adapter, "%s (OpenGL %s)", (const char *)glGetString(GL_RENDERER),
             (const char *)glGetString(GL_VERSION));
    fprintf(stderr, "gs: %s, swap interval %d\n", adapter, s->interval);
    GLuint vs = shader(GL_VERTEX_SHADER, vertex_source), fs = shader(GL_FRAGMENT_SHADER, fragment_source);
    if (!vs || !fs) return 0;
    s->program = glCreateProgram();
    glAttachShader(s->program, vs);
    glAttachShader(s->program, fs);
    glLinkProgram(s->program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = 0;
    glGetProgramiv(s->program, GL_LINK_STATUS, &linked);
    if (!linked) {
        fprintf(stderr, "gs: OpenGL program did not link\n");
        return 0;
    }
    s->rect = glGetUniformLocation(s->program, "rect");
    glUseProgram(s->program);
    glUniform1i(glGetUniformLocation(s->program, "picture"), 0);
    glGenVertexArrays(1, &s->vao);  /* the core profile draws with one bound, even without attributes */
    glGenTextures(1, &s->texture);
    glBindTexture(GL_TEXTURE_2D, s->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    s->failed = 0;
    return 1;
}

DhdrSurface *dhdr_surface_create(const DhdrWindow *w, uint32_t present_flags)
{
    if (!w || w->kind != DHDR_WINDOW_SWITCH || !w->a) {
        fprintf(stderr, "gs: no surface for window kind %u\n", w ? w->kind : 0);
        return NULL;
    }
    DhdrSurface *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->window = (SDL_Window *)(uintptr_t)w->a;
    s->interval = present_flags & (DHDR_PRESENT_NO_TEARING | DHDR_PRESENT_VSYNC) ? 1 : 0;
    g_active_surface = s;
    return s;
}

int dhdr_gl_make_current(DhdrSurface *s) { return current(s); }
DhdrSurface *dhdr_gl_active_surface(void) { return g_active_surface; }

void dhdr_surface_destroy(DhdrSurface *s)
{
    if (!s) return;
    if (g_active_surface == s) g_active_surface = NULL;
    if (s->gl && current(s)) {
        glDeleteTextures(1, &s->texture);
        glDeleteVertexArrays(1, &s->vao);
        glDeleteProgram(s->program);
        SDL_GL_DestroyContext(s->gl);
    }
    free(s);
}

uint32_t dhdr_surface_usage(const DhdrSurface *s) { (void)s; return 0; }

/* pixels (w x h RGBA8 rows) drawn 4:3 in the middle of the window_w x window_h screen. Without
   pixels there is nothing to show yet (no exact display texture): skipped. */
int dhdr_present_exact(DhdrSurface *s, DhdrExact *x, const uint32_t *pixels, uint32_t w, uint32_t h, uint32_t window_w,
                       uint32_t window_h)
{
    (void)x;
    if (!pixels || !w || !h || !window_w || !window_h) return 0;
    if (!current(s)) return -1;
    glBindTexture(GL_TEXTURE_2D, s->texture);
    if (w != s->texture_w || h != s->texture_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        s->texture_w = w, s->texture_h = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    }
    glViewport(0, 0, (GLsizei)window_w, (GLsizei)window_h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    /* the PS2's 4:3 picture, as tall as the screen (narrower if the screen is narrower than 4:3) */
    float width = (float)window_h * 4 / 3, height = (float)window_h;
    if (width > (float)window_w) width = (float)window_w, height = width * 3 / 4;
    float sx = width / (float)window_w, sy = height / (float)window_h;
    glUseProgram(s->program);
    glUniform4f(s->rect, -sx, sy, sx, -sy);
    glBindVertexArray(s->vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    return 1;
}

void dhdr_surface_counter(DhdrSurface *s, int enabled, int ping, double seconds)
{ (void)s; (void)enabled; (void)ping; (void)seconds; }
void dhdr_surface_touch(DhdrSurface *s, const float *items, uint32_t count, float width_mm, float height_mm)
{ (void)s; (void)items; (void)count; (void)width_mm; (void)height_mm; }

void dhdr_surface_present(DhdrSurface *s)
{
    if (s && s->gl && !s->failed) SDL_GL_SwapWindow(s->window);
}
