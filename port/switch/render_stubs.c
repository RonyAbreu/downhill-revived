/* The Switch's renderer entries not drawn yet (finding 144): decoded textures (NX2 step 2),
   texture packs' PNG files, and the test hooks. The device, the exact renderer and presentation
   are render_gl.c's; the native renderer (NX2 step 1) is render_native.c's. */
#include <stddef.h>
#include <stdint.h>
#include "dhd_render.h"
#include "dhd_render_ext.h"

DhdrTexture *dhdr_texture_create(const uint32_t *rgba, uint32_t w, uint32_t h) { (void)rgba; (void)w; (void)h; return NULL; }
DhdrTexture *dhdr_replace_create(const uint8_t *rgba_mips, uint32_t w, uint32_t h, uint32_t levels)
{ (void)rgba_mips; (void)w; (void)h; (void)levels; return NULL; }
void dhdr_texture_retain(DhdrTexture *t) { (void)t; }
void dhdr_texture_release(DhdrTexture *t) { (void)t; }

int dhdr_png_size(const uint8_t *data, size_t size, uint32_t *w, uint32_t *h) { (void)data; (void)size; (void)w; (void)h; return 0; }
int dhdr_png_decode(const uint8_t *data, size_t size, uint8_t *rgba, uint32_t w, uint32_t h, uint32_t opaque)
{ (void)data; (void)size; (void)rgba; (void)w; (void)h; (void)opaque; return 0; }
int dhdr_test_depth_probe(const void *cases, uint32_t count, uint32_t *results) { (void)cases; (void)count; (void)results; return 0; }
const uint32_t *dhdr_test_presented(uint32_t *w, uint32_t *h) { (void)w; (void)h; return NULL; }
