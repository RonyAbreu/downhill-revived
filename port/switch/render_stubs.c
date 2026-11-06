/* The Switch's renderer entries nothing draws yet (finding 167): the test hooks. The device, the
   exact renderer and presentation are render_gl.c's, the native renderer render_native.c's,
   DhdrTexture textures.c's and PNG png_switch.c's. */
#include <stddef.h>
#include <stdint.h>
#include "dhd_render.h"
#include "dhd_render_ext.h"

int dhdr_test_depth_probe(const void *cases, uint32_t count, uint32_t *results) { (void)cases; (void)count; (void)results; return 0; }
const uint32_t *dhdr_test_presented(uint32_t *w, uint32_t *h) { (void)w; (void)h; return NULL; }
