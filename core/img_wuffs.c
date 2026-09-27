/*
 * Wuffs, compiled once, with only the formats cctext reads (PNG, JPEG,
 * GIF, BMP, WebP, QOI) and every Wuffs symbol static to this unit. See
 * core/img_wuffs.h. Plain C: ccc hands a raw .c to the host compiler.
 */
#define WUFFS_IMPLEMENTATION
#define WUFFS_CONFIG__STATIC_FUNCTIONS
#define WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULE__BASE
#define WUFFS_CONFIG__MODULE__ADLER32
#define WUFFS_CONFIG__MODULE__CRC32
#define WUFFS_CONFIG__MODULE__DEFLATE
#define WUFFS_CONFIG__MODULE__ZLIB
#define WUFFS_CONFIG__MODULE__PNG
#define WUFFS_CONFIG__MODULE__JPEG
#define WUFFS_CONFIG__MODULE__LZW
#define WUFFS_CONFIG__MODULE__GIF
#define WUFFS_CONFIG__MODULE__BMP
#define WUFFS_CONFIG__MODULE__VP8
#define WUFFS_CONFIG__MODULE__WEBP
#define WUFFS_CONFIG__MODULE__QOI

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#include "third_party/wuffs/wuffs-v0.4.c"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <stdlib.h>
#include <string.h>

#include "core/img_wuffs.h"

struct RtxWuffsDec {
    wuffs_base__image_decoder *dec;
    int fmt;
    int headed;
    int pb_set;
    int caller_closed;
    uint32_t w, h;
    int nframe;
    wuffs_base__image_config ic;
    wuffs_base__io_buffer src;
    wuffs_base__pixel_buffer pb;
    wuffs_base__rect_ie_u32 last_rect;
    uint8_t last_disposal;
    const char *err;
};

int rtx_wuffs_sniff(const uint8_t *b, size_t n, int closed) {
    int32_t cc;
    if (!b || !n) return RTX_WUFFS_FMT_NONE;
    cc = wuffs_base__magic_number_guess_fourcc(
        wuffs_base__make_slice_u8((uint8_t *)b, n), closed != 0);
    switch (cc) {
    case WUFFS_BASE__FOURCC__PNG: return RTX_WUFFS_FMT_PNG;
    case WUFFS_BASE__FOURCC__JPEG: return RTX_WUFFS_FMT_JPEG;
    case WUFFS_BASE__FOURCC__GIF: return RTX_WUFFS_FMT_GIF;
    case WUFFS_BASE__FOURCC__BMP: return RTX_WUFFS_FMT_BMP;
    case WUFFS_BASE__FOURCC__WEBP: return RTX_WUFFS_FMT_WEBP;
    case WUFFS_BASE__FOURCC__QOI: return RTX_WUFFS_FMT_QOI;
    default: return RTX_WUFFS_FMT_NONE;
    }
}

RtxWuffsDec *rtx_wuffs_open(int fmt) {
    RtxWuffsDec *d;
    wuffs_base__image_decoder *dec = NULL;
    switch (fmt) {
    case RTX_WUFFS_FMT_PNG:
        dec = wuffs_png__decoder__alloc_as__wuffs_base__image_decoder();
        break;
    case RTX_WUFFS_FMT_JPEG:
        dec = wuffs_jpeg__decoder__alloc_as__wuffs_base__image_decoder();
        break;
    case RTX_WUFFS_FMT_GIF:
        dec = wuffs_gif__decoder__alloc_as__wuffs_base__image_decoder();
        break;
    case RTX_WUFFS_FMT_BMP:
        dec = wuffs_bmp__decoder__alloc_as__wuffs_base__image_decoder();
        break;
    case RTX_WUFFS_FMT_WEBP:
        dec = wuffs_webp__decoder__alloc_as__wuffs_base__image_decoder();
        break;
    case RTX_WUFFS_FMT_QOI:
        dec = wuffs_qoi__decoder__alloc_as__wuffs_base__image_decoder();
        break;
    default:
        return NULL;
    }
    if (!dec) return NULL;
    d = (RtxWuffsDec *)calloc(1, sizeof *d);
    if (!d) {
        free(dec);
        return NULL;
    }
    d->dec = dec;
    d->fmt = fmt;
    d->err = "";
    return d;
}

void rtx_wuffs_close(RtxWuffsDec *d) {
    if (!d) return;
    free(d->dec);
    free(d);
}

const char *rtx_wuffs_err(const RtxWuffsDec *d) {
    return d && d->err ? d->err : "";
}

/* Point the reader at b[0, n) keeping its position; the first call opens
 * one slice. */
static void rtx_wuffs_bind(RtxWuffsDec *d, const uint8_t *b, size_t n, int closed) {
    size_t wi = d->src.meta.wi;
    d->src.data = wuffs_base__make_slice_u8((uint8_t *)b, n);
    if (wi == 0) wi = n < RTX_WUFFS_CHUNK ? n : RTX_WUFFS_CHUNK;
    if (wi > n) wi = n;
    d->src.meta.wi = wi;
    d->caller_closed = closed;
    d->src.meta.closed = closed && wi == n;
}

/* Open the next slice. 0 = more input; RTX_WUFFS_TRUNC / MORE when the
 * range is used up; RTX_WUFFS_CANCEL when asked to stop. */
static int rtx_wuffs_feed(RtxWuffsDec *d, const _Atomic int *cancel) {
    size_t n = d->src.data.len, wi = d->src.meta.wi;
    if (cancel && atomic_load_explicit(cancel, memory_order_relaxed)) return RTX_WUFFS_CANCEL;
    if (wi >= n) return d->caller_closed ? RTX_WUFFS_TRUNC : RTX_WUFFS_MORE;
    wi += RTX_WUFFS_CHUNK;
    if (wi > n || wi < d->src.meta.wi) wi = n;
    d->src.meta.wi = wi;
    d->src.meta.closed = d->caller_closed && wi == n;
    return 0;
}

int rtx_wuffs_head(RtxWuffsDec *d, const uint8_t *b, size_t n, int closed,
                   uint32_t *w, uint32_t *h, uint64_t *workbuf) {
    if (!d || !b) return RTX_WUFFS_CORRUPT;
    if (d->headed) {
        if (w) *w = d->w;
        if (h) *h = d->h;
        if (workbuf) *workbuf = wuffs_base__image_decoder__workbuf_len(d->dec).max_incl;
        return RTX_WUFFS_OK;
    }
    rtx_wuffs_bind(d, b, n, closed);
    for (;;) {
        wuffs_base__status st =
            wuffs_base__image_decoder__decode_image_config(d->dec, &d->ic, &d->src);
        int fr;
        if (!st.repr) break;
        if (st.repr != wuffs_base__suspension__short_read) {
            d->err = st.repr;
            return RTX_WUFFS_CORRUPT;
        }
        fr = rtx_wuffs_feed(d, NULL);
        if (fr) {
            d->err = "truncated header";
            return fr;
        }
    }
    d->w = wuffs_base__pixel_config__width(&d->ic.pixcfg);
    d->h = wuffs_base__pixel_config__height(&d->ic.pixcfg);
    d->headed = 1;
    if (w) *w = d->w;
    if (h) *h = d->h;
    if (workbuf) *workbuf = wuffs_base__image_decoder__workbuf_len(d->dec).max_incl;
    return RTX_WUFFS_OK;
}

static void rtx_wuffs_rect_zero(uint8_t *px, uint32_t w, uint32_t h,
                                wuffs_base__rect_ie_u32 r) {
    uint32_t y;
    if (r.max_excl_x > w) r.max_excl_x = w;
    if (r.max_excl_y > h) r.max_excl_y = h;
    if (r.min_incl_x >= r.max_excl_x || r.min_incl_y >= r.max_excl_y) return;
    for (y = r.min_incl_y; y < r.max_excl_y; y++)
        memset(px + ((size_t)y * w + r.min_incl_x) * 4, 0,
               (size_t)(r.max_excl_x - r.min_incl_x) * 4);
}

static void rtx_wuffs_rect_copy(uint8_t *dst, const uint8_t *src, uint32_t w,
                                uint32_t h, wuffs_base__rect_ie_u32 r) {
    uint32_t y;
    if (r.max_excl_x > w) r.max_excl_x = w;
    if (r.max_excl_y > h) r.max_excl_y = h;
    if (r.min_incl_x >= r.max_excl_x || r.min_incl_y >= r.max_excl_y) return;
    for (y = r.min_incl_y; y < r.max_excl_y; y++) {
        size_t o = ((size_t)y * w + r.min_incl_x) * 4;
        memcpy(dst + o, src + o, (size_t)(r.max_excl_x - r.min_incl_x) * 4);
    }
}

int rtx_wuffs_frame(RtxWuffsDec *d, const uint8_t *b, size_t n,
                    uint8_t *canvas, uint8_t *prev, uint8_t *work,
                    uint64_t workbuf, uint32_t *delay_ms,
                    const _Atomic int *cancel) {
    wuffs_base__frame_config fc = wuffs_base__null_frame_config();
    wuffs_base__pixel_blend blend;
    size_t plen;
    if (delay_ms) *delay_ms = 0;
    if (!d || !d->headed || !canvas) return RTX_WUFFS_CORRUPT;
    plen = (size_t)d->w * d->h * 4;
    d->src.data = wuffs_base__make_slice_u8((uint8_t *)b, n);
    if (!d->pb_set) {
        wuffs_base__status st;
        wuffs_base__pixel_config__set(&d->ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL,
                                      WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, d->w, d->h);
        st = wuffs_base__pixel_buffer__set_from_slice(
            &d->pb, &d->ic.pixcfg, wuffs_base__make_slice_u8(canvas, plen));
        if (st.repr) {
            d->err = st.repr;
            return RTX_WUFFS_CORRUPT;
        }
        memset(canvas, 0, plen);
        d->pb_set = 1;
    } else if (d->nframe > 0) {
        /* The last frame's disposal, before this one draws. */
        if (d->last_disposal == WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_BACKGROUND)
            rtx_wuffs_rect_zero(canvas, d->w, d->h, d->last_rect);
        else if (d->last_disposal == WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_PREVIOUS) {
            if (prev) rtx_wuffs_rect_copy(canvas, prev, d->w, d->h, d->last_rect);
            else rtx_wuffs_rect_zero(canvas, d->w, d->h, d->last_rect);
        }
    }
    for (;;) {
        wuffs_base__status st =
            wuffs_base__image_decoder__decode_frame_config(d->dec, &fc, &d->src);
        int fr;
        if (!st.repr) break;
        if (st.repr == wuffs_base__note__end_of_data) return RTX_WUFFS_END;
        if (st.repr != wuffs_base__suspension__short_read) {
            d->err = st.repr;
            return RTX_WUFFS_CORRUPT;
        }
        fr = rtx_wuffs_feed(d, cancel);
        if (fr) {
            if (fr == RTX_WUFFS_TRUNC && d->nframe > 0) return RTX_WUFFS_END;
            d->err = fr == RTX_WUFFS_CANCEL ? "cancelled" : "truncated";
            return fr == RTX_WUFFS_MORE ? RTX_WUFFS_TRUNC : fr;
        }
    }
    d->last_rect = wuffs_base__frame_config__bounds(&fc);
    d->last_disposal = wuffs_base__frame_config__disposal(&fc);
    if (d->last_disposal == WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_PREVIOUS && prev)
        memcpy(prev, canvas, plen);
    blend = (d->nframe == 0 || wuffs_base__frame_config__overwrite_instead_of_blend(&fc))
                ? WUFFS_BASE__PIXEL_BLEND__SRC
                : WUFFS_BASE__PIXEL_BLEND__SRC_OVER;
    for (;;) {
        wuffs_base__status st = wuffs_base__image_decoder__decode_frame(
            d->dec, &d->pb, &d->src, blend, wuffs_base__make_slice_u8(work, (size_t)workbuf),
            NULL);
        int fr;
        if (!st.repr) break;
        if (st.repr != wuffs_base__suspension__short_read) {
            d->err = st.repr;
            return RTX_WUFFS_CORRUPT;
        }
        fr = rtx_wuffs_feed(d, cancel);
        if (fr) {
            d->err = fr == RTX_WUFFS_CANCEL ? "cancelled" : "truncated";
            return fr == RTX_WUFFS_MORE ? RTX_WUFFS_TRUNC : fr;
        }
    }
    if (delay_ms) {
        int64_t fl = (int64_t)wuffs_base__frame_config__duration(&fc);
        *delay_ms = fl > 0 ? (uint32_t)(fl / (WUFFS_BASE__FLICKS_PER_SECOND / 1000)) : 0;
    }
    d->nframe++;
    return RTX_WUFFS_OK;
}
