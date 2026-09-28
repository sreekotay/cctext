/*
 * Terminal image encoders (docs/images.md "Terminal"). Pure C, no state,
 * any thread: the cctext encode jobs call these on pixels that came from
 * the Wuffs loader (never on a file's raw bytes).
 *
 *   - rtx_timg_scale: linear-light area resample of straight RGBA to a
 *     target size (a crop of it), the one scaler every encoder uses;
 *   - rtx_timg_blocks: Unicode block art (half blocks, quadrants or
 *     sextants) in 24-bit colour or the xterm 256-colour cube, optional
 *     Floyd-Steinberg dither;
 *   - rtx_timg_sixel: median-cut palette (<= 256) + sixel bands with RLE;
 *   - rtx_timg_png: an RGBA PNG (iTerm2 inline images);
 *   - rtx_timg_zlib: a zlib stream (fixed-Huffman deflate + LZ77), for
 *     kitty's o=z and PNG;
 *   - rtx_timg_b64, rtx_timg_kitty_ph (a kitty Unicode placeholder cell).
 */
#ifndef RTX_IMG_TERM_H
#define RTX_IMG_TERM_H

#include <stddef.h>
#include <stdint.h>

/* A cell colour: -1 = the terminal's default, 0..255 = an xterm palette
 * index, RTX_TIMG_RGB(r, g, b) = 24-bit. */
#define RTX_TIMG_RGB(r, g, b) \
    ((int32_t)(0x1000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b)))
#define RTX_TIMG_IS_RGB(c) ((c) >= 0x1000000)

enum {
    RTX_TIMG_HALF = 0,     /* ▀ with fg / bg: 1 x 2 sub-pixels a cell */
    RTX_TIMG_QUAD = 1,     /* quadrants: 2 x 2 */
    RTX_TIMG_SEXT = 2      /* sextants (U+1FB00 block): 2 x 3 */
};

enum {
    RTX_TIMG_TRUE = 0,     /* 24-bit SGR */
    RTX_TIMG_256 = 1       /* xterm 256 (the cube and the grey ramp, 16-255) */
};

typedef struct {
    uint32_t cp;           /* U+0020, U+2580.., U+1FB00.. */
    int32_t fg, bg;
} RtxTimgCell;

/* Linear-light area resample. src: straight RGBA, w x h, stride bytes.
 * The whole source maps onto a tw x th target; the output is its crop
 * [cx, cx + cw) x [cy, cy + ch) as straight RGBA (sRGB), cw * 4 stride.
 * Down: each target pixel averages the source area it covers (premultiplied,
 * in linear light); up: nearest (a box). 0 on a bad argument / no memory. */
int rtx_timg_scale(const uint8_t *src, uint32_t w, uint32_t h, uint32_t stride,
                   uint32_t tw, uint32_t th, uint32_t cx, uint32_t cy, uint32_t cw,
                   uint32_t ch, uint8_t *dst);

/* Block art of a cols x rows cell box. rgba (straight, w x h) is the
 * picture; it paints disp_w x disp_h pixels (0: w x h) at cell_w x cell_h
 * pixels a cell, from the box's top-left (it may cover less than the box:
 * the rest is clear). Clear sub-pixels (alpha < 50 %) keep the terminal's
 * background. out: cols * rows cells, row-major. 0 on failure. */
int rtx_timg_blocks(const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t stride,
                    uint32_t disp_w, uint32_t disp_h, uint32_t cell_w, uint32_t cell_h,
                    uint32_t cols, uint32_t rows, int glyphs, int colors, int dither,
                    RtxTimgCell *out);

/* Composite translucent pixels (0 < alpha < 255) of a straight RGBA
 * picture over bg (0xRRGGBB), in place; alpha is kept (block art and
 * sixel still cut at 50 %, the colour of what stays is the blend). */
void rtx_timg_matte(uint8_t *rgba, uint32_t w, uint32_t h, uint32_t stride, uint32_t bg);

/* Nearest xterm palette index (16-255) for an sRGB colour. */
int rtx_timg_256(uint8_t r, uint8_t g, uint8_t b);
/* The sRGB colour of xterm palette index i (16-255; 0-15 as xterm's). */
void rtx_timg_256_rgb(int i, uint8_t *r, uint8_t *g, uint8_t *b);

/* UTF-8 of a code point; returns bytes (1-4). */
int rtx_timg_utf8(uint32_t cp, char *out);

/* SGR for a cell's colours ("\x1b[0;38;2;r;g;b;48;5;n m"); returns bytes. */
int rtx_timg_sgr(int32_t fg, int32_t bg, char *out, size_t cap);

/* Sixel (DCS ... ST) of a straight RGBA picture: a median-cut palette of
 * at most ncolors (2-256), transparent pixels left untouched (P2 = 1).
 * *out is malloc'd. `cancel` (may be NULL) is polled between bands. */
int rtx_timg_sixel(const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t stride,
                   int ncolors, const volatile int *cancel, char **out, size_t *n);

/* PNG (8-bit RGBA) of a straight RGBA picture; *out malloc'd. */
int rtx_timg_png(const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t stride,
                 uint8_t **out, size_t *n);

/* zlib stream of b[0, n) (fixed-Huffman deflate, LZ77 over 32 KiB);
 * *out malloc'd. */
int rtx_timg_zlib(const uint8_t *b, size_t n, uint8_t **out, size_t *on);

/* Base64 of b[0, n) into out (cap >= 4 * ((n + 2) / 3) + 1); returns bytes. */
size_t rtx_timg_b64(const uint8_t *b, size_t n, char *out);
size_t rtx_timg_b64_len(size_t n);

/* A kitty Unicode placeholder cell: U+10EEEE, the row and column
 * diacritics, and (msb != 0) a third one for the image id's top byte.
 * Returns bytes (<= 14). row / col < RTX_TIMG_PH_MAX. */
#define RTX_TIMG_PH_MAX 297
int rtx_timg_kitty_ph(int row, int col, int msb, char *out);
/* Diacritic index of a code point (tests, the fake terminal): -1 none. */
int rtx_timg_kitty_diac(uint32_t cp);

#endif /* RTX_IMG_TERM_H */
