/* The Switch's PNG decoding (finding 144): dhd_render.h's dhdr_png_size and dhdr_png_decode,
   which the texture packs (port/gs/texpack.c) and the button icons (port/gs/button_icons.c) ask
   for. The other targets get these from the Rust `png` crate through port/render; there is no
   Rust on Horizon, so this is libpng, which devkitPro packages as switch-libpng.

   What the ABI asks for, and what the Rust path does (port/render/src/png_decode.rs): RGBA8 rows
   from any colour type, bit depth and interlacing, 16-bit samples cut to their high byte, and a
   PNG with neither an alpha channel nor tRNS given `opaque` as its alpha. libpng's own transforms
   do all of it. Any thread: nothing here is shared. */
#include <png.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dhd_render.h"

typedef struct Reader { const uint8_t *data; size_t size, at; } Reader;

static void read_bytes(png_structp png, png_bytep out, png_size_t want)
{
    Reader *r = (Reader *)png_get_io_ptr(png);
    if (!r || r->at + want > r->size) {
        png_error(png, "past the end of the file");   /* longjmps; never returns */
        return;
    }
    memcpy(out, r->data + r->at, want);
    r->at += want;
}

static void on_error(png_structp png, png_const_charp message)
{
    fprintf(stderr, "png: %s\n", message ? message : "failed");
    longjmp(png_jmpbuf(png), 1);
}

static void on_warning(png_structp png, png_const_charp message)
{
    (void)png; (void)message;   /* a warning is not a reason to refuse the file */
}

/* png and info freed, and 0 returned, so every failure below is one line. */
static int give_up(png_structp png, png_infop info)
{
    png_destroy_read_struct(&png, info ? &info : NULL, NULL);
    return 0;
}

/* The reader, up to the header. 0 (and nothing left to free) when it is not a PNG. */
static int open_png(const uint8_t *data, size_t size, Reader *r, png_structp *png_out, png_infop *info_out)
{
    if (!data || size < 8 || png_sig_cmp((png_const_bytep)data, 0, 8)) return 0;
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, on_error, on_warning);
    if (!png) return 0;
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, NULL, NULL); return 0; }
    r->data = data; r->size = size; r->at = 0;
    png_set_read_fn(png, r, read_bytes);
    *png_out = png; *info_out = info;
    return 1;
}

int dhdr_png_size(const uint8_t *data, size_t size, uint32_t *w, uint32_t *h)
{
    Reader r;
    png_structp png;
    png_infop info;
    if (w) *w = 0;
    if (h) *h = 0;
    if (!open_png(data, size, &r, &png, &info)) return 0;
    if (setjmp(png_jmpbuf(png))) return give_up(png, info);
    png_read_info(png, info);
    if (w) *w = png_get_image_width(png, info);
    if (h) *h = png_get_image_height(png, info);
    png_destroy_read_struct(&png, &info, NULL);
    return 1;
}

int dhdr_png_decode(const uint8_t *data, size_t size, uint8_t *rgba, uint32_t w, uint32_t h, uint32_t opaque)
{
    Reader r;
    png_structp png;
    png_infop info;
    /* volatile and declared before the setjmp: a longjmp out of libpng may otherwise leave a
       local written after it indeterminate, and this one owns memory. */
    png_bytep *volatile rows = NULL;
    if (!rgba || !w || !h) return 0;
    if (!open_png(data, size, &r, &png, &info)) return 0;
    if (setjmp(png_jmpbuf(png))) {
        free(rows);
        return give_up(png, info);
    }
    png_read_info(png, info);
    if (png_get_image_width(png, info) != w || png_get_image_height(png, info) != h) {
        fprintf(stderr, "png: %ux%u asked for, %ux%u in the file\n", w, h,
                (unsigned)png_get_image_width(png, info), (unsigned)png_get_image_height(png, info));
        return give_up(png, info);
    }
    png_byte colour = png_get_color_type(png, info);
    int has_trns = png_get_valid(png, info, PNG_INFO_tRNS) != 0;
    if (png_get_bit_depth(png, info) == 16) png_set_strip_16(png);          /* the high byte */
    if (colour == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (colour == PNG_COLOR_TYPE_GRAY && png_get_bit_depth(png, info) < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    if (has_trns) png_set_tRNS_to_alpha(png);
    if (colour == PNG_COLOR_TYPE_GRAY || colour == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
    /* Neither an alpha channel nor tRNS: `opaque` becomes the alpha, as the ABI says. */
    if (!(colour & PNG_COLOR_MASK_ALPHA) && !has_trns)
        png_set_filler(png, opaque & 0xFF, PNG_FILLER_AFTER);
    png_set_interlace_handling(png);
    png_read_update_info(png, info);
    if (png_get_rowbytes(png, info) != (png_size_t)w * 4) {
        fprintf(stderr, "png: %u bytes a row, %u wanted\n", (unsigned)png_get_rowbytes(png, info), w * 4);
        return give_up(png, info);
    }
    rows = (png_bytep *)malloc((size_t)h * sizeof *rows);
    if (!rows) return give_up(png, info);
    for (uint32_t y = 0; y < h; y++) rows[y] = rgba + (size_t)y * w * 4;
    png_read_image(png, rows);
    free(rows);
    png_destroy_read_struct(&png, &info, NULL);
    return 1;
}
