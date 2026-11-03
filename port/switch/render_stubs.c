/* The Switch's renderer entries not drawn yet (finding 144): the native renderer, its textures and
   the texture packs' PNG files refuse, so gpu_rust.c stays on the exact renderer (native_init fails
   and says so) and texture packs are off. The device, the exact renderer and presentation are
   render_gl.c's. NX2's later steps replace these. */
#include <stddef.h>
#include <stdint.h>
#include "dhd_render.h"
#include "dhd_render_ext.h"

DhdrNative *dhdr_native_create(uint32_t flags) { (void)flags; return NULL; }
void dhdr_native_destroy(DhdrNative *n) { (void)n; }
DhdrTexture *dhdr_texture_create(const uint32_t *rgba, uint32_t w, uint32_t h) { (void)rgba; (void)w; (void)h; return NULL; }
DhdrTexture *dhdr_replace_create(const uint8_t *rgba_mips, uint32_t w, uint32_t h, uint32_t levels)
{ (void)rgba_mips; (void)w; (void)h; (void)levels; return NULL; }
void dhdr_texture_retain(DhdrTexture *t) { (void)t; }
void dhdr_texture_release(DhdrTexture *t) { (void)t; }
int dhdr_native_surface(DhdrNative *n, uint32_t index, uint32_t scale, uint32_t w, uint32_t h, uint32_t depth,
                        uint32_t size_out[2])
{ (void)n; (void)index; (void)scale; (void)w; (void)h; (void)depth; (void)size_out; return 0; }
int dhdr_native_picture(DhdrNative *n, const DhdrPicture *p, DhdrPictureResult *out) { (void)n; (void)p; (void)out; return 0; }
int dhdr_native_picture_indexed(DhdrNative *n, const DhdrPicture *p, const uint32_t *indices, uint32_t index_count,
                                DhdrPictureResult *out)
{ (void)n; (void)p; (void)indices; (void)index_count; (void)out; return 0; }
int dhdr_native_present(DhdrNative *n, DhdrSurface *s, const DhdrPresentRect *r) { (void)n; (void)s; (void)r; return 0; }
int dhdr_native_read_begin(DhdrNative *n, uint32_t *w, uint32_t *h) { (void)n; (void)w; (void)h; return 0; }
int dhdr_native_read_finish(DhdrNative *n, uint32_t *pixels) { (void)n; (void)pixels; return 0; }
void dhdr_native_options(DhdrNative *n, uint32_t flags) { (void)n; (void)flags; }
int dhdr_native_zread_start(DhdrNative *n, uint32_t index, const uint32_t rect[4], uint64_t tag)
{ (void)n; (void)index; (void)rect; (void)tag; return 0; }
int dhdr_native_zread_take(DhdrNative *n, uint32_t *out, uint32_t cap, uint32_t rect_out[4], uint64_t *tag_out)
{ (void)n; (void)out; (void)cap; (void)rect_out; (void)tag_out; return 0; }

int dhdr_png_size(const uint8_t *data, size_t size, uint32_t *w, uint32_t *h) { (void)data; (void)size; (void)w; (void)h; return 0; }
int dhdr_png_decode(const uint8_t *data, size_t size, uint8_t *rgba, uint32_t w, uint32_t h, uint32_t opaque)
{ (void)data; (void)size; (void)rgba; (void)w; (void)h; (void)opaque; return 0; }
int dhdr_test_depth_probe(const void *cases, uint32_t count, uint32_t *results) { (void)cases; (void)count; (void)results; return 0; }
const uint32_t *dhdr_test_presented(uint32_t *w, uint32_t *h) { (void)w; (void)h; return NULL; }
