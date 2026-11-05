/* The Switch's renderer entries not drawn yet (finding 144): the texture packs' PNG files
   (NX2 step 2's remainder) and the test hooks. The device, the exact renderer and presentation
   are render_gl.c's; the native renderer (NX2 steps 1 and 2) is render_native.c's, and
   DhdrTexture itself is textures.c's.

   PNG: with no decoder, dhdr_png_size refuses every file, so texpack.c loads no texture pack and
   button_icons.c shows no button icons. The sharp font is not a PNG and does work (it is built
   from the game's own font through dhdr_replace_create). */
#include <stddef.h>
#include <stdint.h>
#include "dhd_render.h"
#include "dhd_render_ext.h"

int dhdr_png_size(const uint8_t *data, size_t size, uint32_t *w, uint32_t *h) { (void)data; (void)size; (void)w; (void)h; return 0; }
int dhdr_png_decode(const uint8_t *data, size_t size, uint8_t *rgba, uint32_t w, uint32_t h, uint32_t opaque)
{ (void)data; (void)size; (void)rgba; (void)w; (void)h; (void)opaque; return 0; }
int dhdr_test_depth_probe(const void *cases, uint32_t count, uint32_t *results) { (void)cases; (void)count; (void)results; return 0; }
const uint32_t *dhdr_test_presented(uint32_t *w, uint32_t *h) { (void)w; (void)h; return NULL; }
