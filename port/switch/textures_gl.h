/* DhdrTexture on the Switch (NX2 step 2, finding 167), shared between textures.c (the objects
   themselves) and render_native.c (the draws that sample them).

   dhd_render.h lets dhdr_texture_create, _retain and _release run on any thread (the GS worker,
   the texture-pack loaders, the render thread), but this port has one OpenGL context on one
   thread. So a DhdrTexture is made as a plain CPU copy of its pixels wherever it is asked for,
   and its GL texture is built the first time the render thread samples it (dhdr_tex_gl); the CPU
   copy is freed then. A texture released anywhere goes on a dead list instead of being deleted,
   and dhdr_tex_collect frees the GL objects on the render thread, between pictures. */
#ifndef SWITCH_TEXTURES_GL_H
#define SWITCH_TEXTURES_GL_H
#include <glad/glad.h>
#include <stdint.h>
#include "dhd_render.h"

/* t's GL texture, uploaded on the first call. RENDER THREAD ONLY, context current.
   0: no texture (t is NULL, or the upload failed; logged once per texture). */
GLuint dhdr_tex_gl(DhdrTexture *t);

/* 1 when t is a replacement (texture pack or sharp font: RGBA8 with mip levels, sampled), 0 when
   it is a decoded texture (R32UI, one level, fetched texel by texel). */
int dhdr_tex_is_replacement(const DhdrTexture *t);

/* Where t's texels start in its GL texture: a slot's origin in an atlas page (DHD_SWITCH_UPLOAD
   = atlas, decoded textures), else 0, 0. */
void dhdr_tex_origin(const DhdrTexture *t, int32_t *x, int32_t *y);

/* The atlas pages made so far and their bytes, for the log. */
void dhdr_tex_atlas_stats(uint64_t *pages, uint64_t *bytes);

/* t's size in texels; levels_out may be NULL. */
void dhdr_tex_size(const DhdrTexture *t, uint32_t *w, uint32_t *h, uint32_t *levels_out);

/* The replacement sampler `index`, as port/render/src/native/shared.rs makes its eight:
   bilinear | U clamped << 1 | V clamped << 2. RENDER THREAD ONLY. */
GLuint dhdr_tex_sampler(unsigned index);

/* A 1 x 1 texture to leave bound where a draw samples nothing, so the shader's sampler always has
   a texture of its own kind bound. RENDER THREAD ONLY. */
GLuint dhdr_tex_dummy_texels(void);   /* R32UI */
GLuint dhdr_tex_dummy_rgba(void);     /* RGBA8 */

/* Deletes the GL objects and pixels of every texture released since the last call.
   RENDER THREAD ONLY, context current; called once per picture. */
void dhdr_tex_collect(void);

/* What the uploads have cost so far (DHD_GL_TIMES): how many, their nanoseconds and their bytes. */
void dhdr_tex_upload_stats(uint64_t *count, uint64_t *ns, uint64_t *bytes);

/* Of that: the nanoseconds spent making new GL textures, and the uploads that reused a pooled
   one (a released texture's GL object of the same kind and size) instead. */
void dhdr_tex_upload_split(uint64_t *alloc_ns, uint64_t *reused);

/* Idle uploads (finding 168): new textures queued for the render thread to upload while it waits
   for the next field. dhdr_tex_idle_waker names what wakes that wait when the queue goes from
   empty to one (NULL: no queue; until it is set, textures are not queued at all).
   dhdr_tex_idle_pending: 1 while the queue holds any. dhdr_tex_idle_upload uploads the oldest
   (RENDER THREAD ONLY, context current) and returns 0 when there was none.
   dhdr_tex_idle_stats: what those uploads have cost, apart from dhdr_tex_upload_stats'. */
void dhdr_tex_idle_waker(void (*wake)(void *), void *arg);
int dhdr_tex_idle_pending(void);
int dhdr_tex_idle_upload(void);
void dhdr_tex_idle_stats(uint64_t *count, uint64_t *ns, uint64_t *bytes);

/* Every GL object textures.c owns (the dead list drained first), for surface teardown. */
void dhdr_tex_shutdown(void);

#endif
