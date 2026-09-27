/*
 * The one door to Wuffs (third_party/wuffs, Apache-2.0 / MIT): a plain C
 * translation unit (core/img_wuffs.c) compiles the single-file release
 * with only the image formats cctext reads, and exposes this small API.
 * No other file includes wuffs; core/img.ccs owns limits, EXIF, scaling
 * and the cache. docs/images.md.
 *
 * Input is a byte range already in memory (a file capped at the file
 * limit, a data: URI, a fetched cache file). It is handed to the decoder
 * in RTX_WUFFS_CHUNK slices; between slices the decode polls `cancel`,
 * so a cancelled job stops within one slice's worth of work.
 */
#ifndef RTX_IMG_WUFFS_H
#define RTX_IMG_WUFFS_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define RTX_WUFFS_CHUNK (256u << 10)

enum {
    RTX_WUFFS_OK = 0,
    RTX_WUFFS_MORE = 1,        /* header needs more bytes than given */
    RTX_WUFFS_UNKNOWN = -1,    /* not a format we decode */
    RTX_WUFFS_CORRUPT = -2,    /* the decoder rejected the bytes */
    RTX_WUFFS_TRUNC = -3,      /* ended before the image did */
    RTX_WUFFS_NOMEM = -4,
    RTX_WUFFS_CANCEL = -5,
    RTX_WUFFS_END = -6         /* no more frames */
};

/* Formats (RtxImgInfo.fmt). */
enum {
    RTX_WUFFS_FMT_NONE = 0,
    RTX_WUFFS_FMT_PNG,
    RTX_WUFFS_FMT_JPEG,
    RTX_WUFFS_FMT_GIF,
    RTX_WUFFS_FMT_BMP,
    RTX_WUFFS_FMT_WEBP,
    RTX_WUFFS_FMT_QOI
};

/* Sniff the format from a prefix. RTX_WUFFS_FMT_NONE: not ours (or too
 * short to tell when `closed` is 0). */
int rtx_wuffs_sniff(const uint8_t *b, size_t n, int closed);

typedef struct RtxWuffsDec RtxWuffsDec;

/* A decoder for `fmt` (malloc'd; NULL on no memory / unknown fmt). */
RtxWuffsDec *rtx_wuffs_open(int fmt);
void rtx_wuffs_close(RtxWuffsDec *d);

/* Read the image config from b[0, n). `closed`: n is the whole input.
 * Fills the canvas size and the decoder's work buffer need (bytes).
 * RTX_WUFFS_MORE when the header runs past n and !closed. Allocates
 * nothing but the decoder's own state: the caller checks its limits on
 * w / h / workbuf before asking for any pixels. */
int rtx_wuffs_head(RtxWuffsDec *d, const uint8_t *b, size_t n, int closed,
                   uint32_t *w, uint32_t *h, uint64_t *workbuf);

/* Decode the next frame onto `canvas` (BGRA premultiplied, w * h * 4
 * bytes, stride w * 4, as rtx_wuffs_head sized it). The first frame
 * starts from a zeroed canvas; later frames composite per their
 * disposal / blend (GIF, animated WebP where Wuffs reads it). `prev` is
 * scratch of the canvas size for "restore previous" (NULL: treated as
 * restore background). *delay_ms: this frame's duration. `work` is
 * `workbuf` bytes. Polls *cancel between input slices. RTX_WUFFS_END
 * when there is no further frame. Uses the input the head call saw
 * (b / n must be the same range). */
int rtx_wuffs_frame(RtxWuffsDec *d, const uint8_t *b, size_t n,
                    uint8_t *canvas, uint8_t *prev, uint8_t *work,
                    uint64_t workbuf, uint32_t *delay_ms,
                    const _Atomic int *cancel);

/* The last decoder error text (static; "" when none). */
const char *rtx_wuffs_err(const RtxWuffsDec *d);

#endif /* RTX_IMG_WUFFS_H */
