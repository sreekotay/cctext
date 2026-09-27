/*
 * The sandboxed renderer helper's client (docs/images.md, "Renderer" and
 * "Mermaid"). Owner: core/img_svg.c. Plain C (no ccc header), called by
 * core/img.ccs from its background jobs; any thread.
 *
 * A pool of two helper processes (bin/cctext-render, beside the editor
 * binary): slot 0 renders SVG, slot 1 Mermaid (and later math), so a
 * slow diagram never queues behind SVG and raster work. Each is spawned
 * on its first request, kept for later ones, respawned after a crash or a
 * timeout (the editor kills it), and told to quit at exit. Requests on a
 * slot are serialized; every request carries a generation stamp and a
 * reply with another stamp is dropped (a request abandoned on cancel).
 * The time budget is the editor's: past it the helper is killed (for
 * Mermaid the engine's own interrupt comes first, 2 s earlier).
 *
 * Content-hash cache: the intrinsic size of a document (by FNV-1a 64 of
 * its bytes; for Mermaid, of the options line, the source and the
 * renderer version) is kept in memory and, with the rendered pixels per
 * size, in a cache directory (the safe home's img/svg, img/mermaid) so a
 * later session lays out and paints without the helper. Sizes and pixels
 * are separate files: layout never reads pixels.
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
#define RTX_SVG_ETIMEOUT (-20)  /* past the time budget: killed / interrupted */
#define RTX_SVG_ENONE (-21)     /* no renderer (missing, or refused on this platform) */
#define RTX_SVG_EBIG (-22)      /* input over the size limit */
#define RTX_SVG_ESCRIPT (-23)   /* Mermaid: the diagram does not parse (message) */
#define RTX_SVG_ELARGE (-24)    /* Mermaid: over the node cap (message) */

/* The renderer version mixed into Mermaid cache keys (memory and disk):
 * bump it when the helper's output for the same source changes (a new
 * mermaid.min.js, DOM shim or font). */
#define RTX_MERMAID_VERSION "cctext-render 2; mermaid 12.0.0; dom 1"

/* Helper slots. */
enum {
    RTX_RENDER_SVG = 0,
    RTX_RENDER_MERMAID = 1,
    RTX_RENDER_SLOTS = 2
};

typedef struct {
    uint64_t input_max;    /* bytes one request may carry */
    uint64_t px_max;       /* output pixels (w * h) */
    uint32_t timeout_ms;   /* one request, helper included */
    uint64_t cache_max;    /* disk cache bytes (0: no disk cache) */
    const char *cache_dir; /* NULL: no disk cache */
    /* Mermaid only */
    uint32_t node_max;     /* refuse larger diagrams (0: no cap) */
    uint32_t recycle_mb;   /* helper engine restarts past this heap (0: its 256) */
    uint32_t recycle_jobs; /* ... and every N diagrams (0: never) */
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

/* Mermaid: `payload` is the options JSON line, a newline, then the
 * diagram source (render/cr_proto.h). The same two steps; on
 * RTX_SVG_ESCRIPT / ELARGE / ETIMEOUT the helper's message (one line:
 * "Parse error on line 3: ...", "diagram too large to render (N nodes,
 * limit M)") is copied into msg. */
int rtx_mermaid_size(const uint8_t *payload, size_t n, const RtxSvgCfg *cfg,
                     const _Atomic int *cancel, float *w_css, float *h_css, char *msg,
                     size_t msgcap);
int rtx_mermaid_render(const uint8_t *payload, size_t n, const RtxSvgCfg *cfg, uint32_t pw,
                       uint32_t ph, const _Atomic int *cancel, uint8_t **out, char *msg,
                       size_t msgcap);

/* Quit every helper (atexit does it too). */
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
/* The SVG slot's counters (rtx_render_stats for either). */
void rtx_svg_stats(RtxSvgStats *out);
void rtx_render_stats(int slot, RtxSvgStats *out);
/* Forget the in-memory size cache (tests: prove the disk cache). */
void rtx_svg_forget_memory(void);

#endif /* RTX_IMG_SVG_H */
