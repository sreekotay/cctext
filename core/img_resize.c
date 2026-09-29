/*
 * The picture resampler: see core/img_resize.h. Plain C: ccc hands a raw
 * .c to the host compiler. stb_image_resize2 (third_party/stb, v2.18) is
 * compiled here once, every symbol static.
 */
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_STATIC
/* An internal check that fails must not abort the editor over a picture. */
#define STBIR_ASSERT(x) ((void)0)
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
#include "third_party/stb/stb_image_resize2.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "core/img_resize.h"
#include "core/img_wuffs.h"

static int rs_cancelled(const _Atomic int *cancel) {
    return cancel && atomic_load_explicit(cancel, memory_order_relaxed);
}

/* Coded (cx, cy) of displayed (x, y) under EXIF orientation o, for a
 * coded image cw x ch (core/img.ccs has the same map). */
static void rs_orient_map(int o, uint32_t cw, uint32_t ch, uint32_t x, uint32_t y, uint32_t *cx,
                          uint32_t *cy) {
    switch (o) {
    case 2: *cx = cw - 1 - x; *cy = y; break;
    case 3: *cx = cw - 1 - x; *cy = ch - 1 - y; break;
    case 4: *cx = x; *cy = ch - 1 - y; break;
    case 5: *cx = y; *cy = x; break;
    case 6: *cx = y; *cy = ch - 1 - x; break;
    case 7: *cx = cw - 1 - y; *cy = ch - 1 - x; break;
    case 8: *cx = cw - 1 - y; *cy = x; break;
    default: *cx = x; *cy = y; break;
    }
}

/* The prefilter's inner loops, alias-free so the compiler vectorises
 * them (clang does at -O2; GCC's -O2 cost model declines, so ask). */
#if defined(__GNUC__) && !defined(__clang__)
#define RS_VEC __attribute__((optimize("tree-vectorize", "vect-cost-model=dynamic")))
#else
#define RS_VEC
#endif

RS_VEC static void rs_row_set(uint16_t *restrict d, const uint8_t *restrict s, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) d[i] = s[i];
}

RS_VEC static void rs_row_add(uint16_t *restrict d, const uint8_t *restrict s, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) d[i] = (uint16_t)(d[i] + s[i]);
}

RS_VEC static void rs_row_widen(uint32_t *restrict d, const uint16_t *restrict s, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) d[i] += s[i];
}

/* The four channel sums of n pixels of 32-bit sums (no overflow: the
 * caller checked kx * ky * 255). */
RS_VEC static void rs_hsum(uint32_t *restrict out, const uint32_t *restrict a, uint32_t n) {
    uint32_t s0 = 0, s1 = 0, s2 = 0, s3 = 0, x;
    for (x = 0; x < n; x++, a += 4) {
        s0 += a[0];
        s1 += a[1];
        s2 += a[2];
        s3 += a[3];
    }
    out[0] = s0;
    out[1] = s1;
    out[2] = s2;
    out[3] = s3;
}

/* Box prefilter: kx x ky blocks of src averaged into pw x ph (the last
 * block of a row or column averages what it covers). Exact integer sums:
 * the ky rows of a block row are added column by column (one vectorised
 * pass over the canvas), then each block's kx columns. ky * 255 fits 32
 * bits (ky <= 16384), and so does kx * ky * 255 summed in 64. */
static int rs_box(const uint8_t *src, uint32_t cw, uint32_t ch, size_t stride, uint32_t kx,
                  uint32_t ky, uint8_t *out, uint32_t pw, uint32_t ph, const _Atomic int *cancel) {
    size_t rn = (size_t)cw * 4;
    uint32_t *acc = (uint32_t *)malloc(rn * sizeof(uint32_t));
    uint16_t *a16 = (uint16_t *)malloc(rn * sizeof(uint16_t));
    uint32_t py;
    int wide = (uint64_t)kx * ky * 255u > 0xffffffffu;
    if (!acc || !a16) {
        free(acc);
        free(a16);
        return RTX_WUFFS_NOMEM;
    }
    for (py = 0; py < ph; py++) {
        uint32_t y0 = py * ky, y1 = y0 + ky, y, bx;
        uint8_t *o = out + (size_t)py * pw * 4;
        if (y1 > ch) y1 = ch;
        if (rs_cancelled(cancel)) {
            free(acc);
            free(a16);
            return RTX_WUFFS_CANCEL;
        }
        memset(acc, 0, rn * sizeof(uint32_t));
        /* Rows in runs of at most 257 into 16-bit sums (257 * 255 fits),
         * the cheap widening, then into the 32-bit sums. */
        for (y = y0; y < y1;) {
            uint32_t ye = y + 257 > y1 ? y1 : y + 257;
            rs_row_set(a16, src + (size_t)y * stride, rn);
            for (y++; y < ye; y++) rs_row_add(a16, src + (size_t)y * stride, rn);
            rs_row_widen(acc, a16, rn);
        }
        for (bx = 0; bx < pw; bx++) {
            uint32_t x0 = bx * kx, x1 = x0 + kx, k;
            uint64_t sum[4];
            double inv;
            if (x1 > cw) x1 = cw;
            if (wide) {
                uint32_t x;
                sum[0] = sum[1] = sum[2] = sum[3] = 0;
                for (x = x0; x < x1; x++)
                    for (k = 0; k < 4; k++) sum[k] += acc[(size_t)x * 4 + k];
            } else {
                uint32_t s32[4];
                rs_hsum(s32, acc + (size_t)x0 * 4, x1 - x0);
                for (k = 0; k < 4; k++) sum[k] = s32[k];
            }
            /* A multiply, not four divisions: those were most of the cost. */
            inv = 1.0 / ((double)(x1 - x0) * (double)(y1 - y0));
            for (k = 0; k < 4; k++) o[bx * 4 + k] = (uint8_t)((double)sum[k] * inv + 0.5);
        }
    }
    free(acc);
    free(a16);
    return RTX_WUFFS_OK;
}

static stbir_filter rs_filter(int f) {
    switch (f) {
    case RTX_RS_MITCHELL: return STBIR_FILTER_MITCHELL;
    case RTX_RS_TRIANGLE: return STBIR_FILTER_TRIANGLE;
    case RTX_RS_BOX: return STBIR_FILTER_BOX;
    default: return STBIR_FILTER_CATMULLROM;
    }
}

int rtx_img_resize(const uint8_t *src, uint32_t cw, uint32_t ch, size_t stride, int orient,
                   uint8_t *dst, uint32_t w, uint32_t h, const _Atomic int *cancel,
                   const RtxResizeOpt *opt) {
    RtxResizeOpt dflt = {RTX_RS_CATROM, 0, 0};
    uint32_t tw, th, kx = 1, ky = 1, pw = cw, ph = ch;
    const uint8_t *in = src;
    size_t in_stride = stride;
    uint8_t *pre = NULL, *out, *turn = NULL;
    STBIR_RESIZE r;
    int rc = RTX_WUFFS_OK, splits, i;
    if (!opt) opt = &dflt;
    if (!src || !dst || !w || !h || !cw || !ch || stride < (size_t)cw * 4) return RTX_WUFFS_CORRUPT;
    if (cw > 0x7fffffffu / 4 || w > 0x7fffffffu / 4 || h > 0x7fffffffu || ch > 0x7fffffffu)
        return RTX_WUFFS_CORRUPT;
    if (orient < 1 || orient > 8) orient = 1;
    /* The target in the canvas's coded orientation. */
    tw = orient >= 5 ? h : w;
    th = orient >= 5 ? w : h;
    if (orient != 1) {
        turn = (uint8_t *)malloc((size_t)tw * th * 4);
        if (!turn) return RTX_WUFFS_NOMEM;
        out = turn;
    } else {
        out = dst;
    }
    if (tw == cw && th == ch) {
        /* 1:1: a copy (stb would too, less directly). */
        uint32_t y;
        for (y = 0; y < ch; y++)
            memcpy(out + (size_t)y * tw * 4, src + (size_t)y * stride, (size_t)cw * 4);
        goto turned;
    }
    if (!opt->no_prefilter) {
        /* Measured: below a 6:1 shrink the box pass costs more than it
         * saves the cubic (docs/images.md "Scaling"). */
        kx = cw / tw / 2;
        ky = ch / th / 2;
        if (kx < 3) kx = 1;
        if (ky < 3) ky = 1;
    }
    if (kx > 1 || ky > 1) {
        pw = (cw + kx - 1) / kx;
        ph = (ch + ky - 1) / ky;
        pre = (uint8_t *)malloc((size_t)pw * ph * 4);
        if (!pre) {
            rc = RTX_WUFFS_NOMEM;
            goto done;
        }
        rc = rs_box(src, cw, ch, stride, kx, ky, pre, pw, ph, cancel);
        if (rc != RTX_WUFFS_OK) goto done;
        in = pre;
        in_stride = (size_t)pw * 4;
    }
    stbir_resize_init(&r, in, (int)pw, (int)ph, (int)in_stride, out, (int)tw, (int)th, (int)tw * 4,
                      STBIR_BGRA_PM, opt->linear ? STBIR_TYPE_UINT8_SRGB : STBIR_TYPE_UINT8);
    stbir_set_edgemodes(&r, STBIR_EDGE_CLAMP, STBIR_EDGE_CLAMP);
    stbir_set_filters(&r, rs_filter(opt->filter), rs_filter(opt->filter));
    if (kx > 1 || ky > 1) {
        /* The last block may be partial: the canvas ends inside it. */
        stbir_set_input_subrect(&r, 0.0, 0.0, ((double)cw / kx) / pw, ((double)ch / ky) / ph);
    }
    /* Bands of about 64 output rows: cancel lands between them. */
    splits = (int)(th / 64);
    if (splits < 1) splits = 1;
    if (splits > 64) splits = 64;
    splits = stbir_build_samplers_with_splits(&r, splits);
    if (splits < 1) {
        rc = RTX_WUFFS_NOMEM;
        goto done;
    }
    for (i = 0; i < splits; i++) {
        if (rs_cancelled(cancel)) {
            rc = RTX_WUFFS_CANCEL;
            break;
        }
        if (!stbir_resize_extended_split(&r, i, 1)) {
            rc = RTX_WUFFS_NOMEM;
            break;
        }
    }
    stbir_free_samplers(&r);
    if (rc != RTX_WUFFS_OK) goto done;
    {
        /* A cubic overshoots at hard edges: keep every colour within its
         * alpha (valid premultiplied pixels for the blit). */
        size_t n = (size_t)tw * th, k;
        for (k = 0; k < n; k++) {
            uint8_t *p = out + k * 4, a = p[3];
            if (p[0] > a) p[0] = a;
            if (p[1] > a) p[1] = a;
            if (p[2] > a) p[2] = a;
        }
    }
turned:
    if (turn) {
        uint32_t x, y;
        for (y = 0; y < h; y++) {
            uint32_t *o = (uint32_t *)(void *)(dst + (size_t)y * w * 4);
            for (x = 0; x < w; x++) {
                uint32_t cx, cy;
                rs_orient_map(orient, tw, th, x, y, &cx, &cy);
                memcpy(&o[x], turn + ((size_t)cy * tw + cx) * 4, 4);
            }
        }
    }
done:
    free(pre);
    free(turn);
    return rc;
}
