/*
 * SVG through the sandboxed renderer helper (docs/images.md, "Renderer").
 * Owner: core/img_svg.c. Plain C (no ccc header), called by core/img.ccs
 * from its background jobs; any thread.
 *
 * One helper process (bin/cctext-render, beside the editor binary) is
 * spawned on the first SVG, kept for later ones, respawned after a crash
 * or a timeout (the editor kills it), and told to quit at exit. Requests
 * are serialized (a pool of one); every request carries a generation
 * stamp and a reply with another stamp is dropped (a request that was
 * abandoned on cancel). The time budget is the editor's: past it the
 * helper is killed.
 *
 * Content-hash cache: the intrinsic size of an SVG (by FNV-1a 64 of its
 * bytes) is kept in memory and, with the rendered pixels per size, in a
 * cache directory (the safe home's img/svg) so a later session lays out
 * and paints cached SVGs without the helper. Sizes and pixels are
 * separate files: layout never reads pixels.
 *
 * Pixels come back as RGBA8 premultiplied (the wire format); the caller
 * converts to the layout it keeps.
 */
#ifndef RTX_IMG_SVG_H
#define RTX_IMG_SVG_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

/* Results: 0 or these (the same numbers as core/img.cch's RTX_IMG_E*;
 * img.ccs checks). */
#define RTX_SVG_OK 0
#define RTX_SVG_ECORRUPT (-2)   /* not an SVG the renderer reads / no size */
#define RTX_SVG_ENOMEM (-4)
#define RTX_SVG_ECANCEL (-5)
#define RTX_SVG_EPX (-8)        /* output over the pixel cap */
#define RTX_SVG_ECRASH (-19)    /* the helper died or broke protocol */
#define RTX_SVG_ETIMEOUT (-20)  /* past the time budget: killed */
#define RTX_SVG_ENONE (-21)     /* no renderer (missing, or refused on this platform) */
#define RTX_SVG_EBIG (-22)      /* input over the SVG size limit */

typedef struct {
    uint64_t input_max;    /* bytes one request may carry */
    uint64_t px_max;       /* output pixels (w * h) */
    uint32_t timeout_ms;   /* one request, helper included */
    uint64_t cache_max;    /* disk cache bytes (0: no disk cache) */
    const char *cache_dir; /* NULL: no disk cache */
} RtxSvgCfg;

/* SVG text? A UTF-8 BOM, blanks, an XML declaration, comments or a DOCTYPE
 * may come first; "<svg" must start an element within b[0, n). */
int rtx_svg_sniff(const uint8_t *b, size_t n);

/* Intrinsic size in CSS px (memory cache, disk cache, then the helper). */
int rtx_svg_size(const uint8_t *b, size_t n, const RtxSvgCfg *cfg, const _Atomic int *cancel,
                 float *w_css, float *h_css);

/* Pixels at exactly pw x ph (the caller fitted the box): RGBA8
 * premultiplied, stride pw * 4, malloc'd into *out. Disk cache first. */
int rtx_svg_render(const uint8_t *b, size_t n, const RtxSvgCfg *cfg, uint32_t pw, uint32_t ph,
                   const _Atomic int *cancel, uint8_t **out);

/* Quit the helper (atexit does it too). */
void rtx_svg_shutdown(void);

/* The helper binary this process would run (RTX_RENDER_BIN overrides;
 * else cctext-render beside the executable, then ../bin/). "" if none. */
const char *rtx_svg_helper_path(void);

typedef struct {
    unsigned spawns;       /* helper processes started */
    unsigned kills;        /* killed on a timeout / cancel */
    unsigned crashes;      /* died or broke protocol */
    unsigned requests;     /* sent */
    unsigned stale;        /* replies dropped for a stale generation */
    unsigned mem_hits, disk_size_hits, disk_px_hits;
    int pid;               /* the live helper (0: none) */
    double last_ready_ms;  /* spawn to hello, last spawn */
} RtxSvgStats;
void rtx_svg_stats(RtxSvgStats *out);
/* Forget the in-memory size cache (tests: prove the disk cache). */
void rtx_svg_forget_memory(void);

#endif /* RTX_IMG_SVG_H */
