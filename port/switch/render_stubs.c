/* The Switch's renderer for milestone NX1 (finding 144): none. The Rust renderer (port/render,
   wgpu) cannot run on Horizon (no Rust std); until the C backend of NX2 exists, every dhdr_*
   entry of dhd_render.h / dhd_render_ext.h is here and refuses. gs_gpu_init then fails, so the
   game runs headless (DHD_HEADLESS, switch_main.c): the EE, VU1 and the GS front end all work, no
   picture is shown. */
#include <stddef.h>
#include <stdint.h>
#include "dhd_render.h"
#include "dhd_render_ext.h"

int dhdr_device_init(const DhdrDeviceDesc *d) { (void)d; return 0; }
const char *dhdr_adapter_name(void) { return "none (Switch, NX1)"; }
uint32_t dhdr_device_features(void) { return 0; }
const char *dhdr_version(void) { return "switch-nx1-null"; }
uint32_t dhdr_device_max_texture(void) { return 0; }
uint32_t dhdr_perf_read(uint64_t *out, uint32_t count) { (void)out; (void)count; return 0; }

DhdrExact *dhdr_exact_create(uint32_t raster_flags) { (void)raster_flags; return NULL; }
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
int dhdr_exact_read_begin(DhdrExact *x, uint32_t buffer, const uint32_t want[16]) { (void)x; (void)buffer; (void)want; return 0; }
int dhdr_exact_read_finish(DhdrExact *x, uint32_t buffer, const uint32_t want[16], void *image)
{ (void)x; (void)buffer; (void)want; (void)image; return 0; }
int dhdr_exact_capture_begin(DhdrExact *x, uint32_t *w, uint32_t *h) { (void)x; (void)w; (void)h; return 0; }
int dhdr_exact_capture_finish(DhdrExact *x, uint32_t *pixels) { (void)x; (void)pixels; return 0; }
void dhdr_exact_stats(const DhdrExact *x, DhdrExactStats *out) { (void)x; (void)out; }

DhdrSurface *dhdr_surface_create(const DhdrWindow *w, uint32_t present_flags) { (void)w; (void)present_flags; return NULL; }
void dhdr_surface_destroy(DhdrSurface *s) { (void)s; }
uint32_t dhdr_surface_usage(const DhdrSurface *s) { (void)s; return 0; }
int dhdr_present_exact(DhdrSurface *s, DhdrExact *x, const uint32_t *pixels, uint32_t w, uint32_t h, uint32_t window_w,
                       uint32_t window_h)
{ (void)s; (void)x; (void)pixels; (void)w; (void)h; (void)window_w; (void)window_h; return -1; }
void dhdr_surface_counter(DhdrSurface *s, int enabled, int ping, double seconds) { (void)s; (void)enabled; (void)ping; (void)seconds; }
void dhdr_surface_touch(DhdrSurface *s, const float *items, uint32_t count, float width_mm, float height_mm)
{ (void)s; (void)items; (void)count; (void)width_mm; (void)height_mm; }
void dhdr_surface_present(DhdrSurface *s) { (void)s; }

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
