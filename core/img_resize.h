/*
 * The picture resampler (docs/images.md "Scaling"): a decoded canvas to
 * the size it paints. Plain C (core/img_resize.c), the only file that
 * includes stb_image_resize2 (third_party/stb, public domain / MIT).
 *
 * Pixels are 4 bytes, premultiplied alpha, alpha in byte 3 (the loader's
 * BGRA premultiplied; any premultiplied order with alpha last works: the
 * channels are filtered alike). Filtering premultiplied values is what
 * keeps a transparent edge from pulling in the colour of its invisible
 * neighbours (no dark fringe).
 *
 * The filter is separable Catmull-Rom (an interpolating cubic: exact at
 * 1:1, sharp text and edges, a small overshoot that is clamped), scaled
 * with the ratio when shrinking so every source pixel is covered (area
 * coverage: a 2:1 checkerboard becomes an even grey). A shrink by 4 or
 * more first box-averages k x k blocks (k = ratio / 2, exact integer
 * area sums) down to 2-3x the target, so a huge canvas costs one pass of
 * additions and the cubic runs on a small image. Resampling is in the
 * file's (sRGB) values, as browsers and the platform blits do.
 *
 * EXIF orientation (1-8) is applied after resampling: a flip or a
 * transpose commutes with a symmetric separable filter, so the canvas is
 * resized in its coded orientation and only the small result is turned.
 */
#ifndef RTX_IMG_RESIZE_H
#define RTX_IMG_RESIZE_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

enum {
    RTX_RS_CATROM = 0,   /* default: Catmull-Rom (B = 0, C = 1/2) */
    RTX_RS_MITCHELL,     /* Mitchell-Netravali (B = C = 1/3): softer, no ringing */
    RTX_RS_TRIANGLE,     /* bilinear / tent */
    RTX_RS_BOX           /* area average */
};

typedef struct {
    int filter;          /* RTX_RS_* */
    int linear;          /* 1: resample in linear light (sRGB decoded); 0: in sRGB values */
    int no_prefilter;    /* 1: never box-prefilter (tests, bench) */
} RtxResizeOpt;

/* Resample src (cw x ch coded pixels, `stride` bytes a row) under EXIF
 * orientation `orient` into dst (w x h displayed pixels, stride w * 4).
 * `opt` NULL: the defaults above. Polls *cancel (NULL: none) between
 * prefilter rows and output bands. Returns 0 (RTX_WUFFS_OK), or
 * RTX_WUFFS_NOMEM / RTX_WUFFS_CANCEL (core/img_wuffs.h) with dst
 * unspecified. Transient memory: the prefiltered image (at most about 9x
 * dst) when shrinking by 4 or more, dst's size again when `orient` is not
 * 1, and the filter's ring buffers (a few rows). */
int rtx_img_resize(const uint8_t *src, uint32_t cw, uint32_t ch, size_t stride, int orient,
                   uint8_t *dst, uint32_t w, uint32_t h, const _Atomic int *cancel,
                   const RtxResizeOpt *opt);

#endif
