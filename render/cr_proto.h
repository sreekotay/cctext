/*
 * cctext-render wire protocol (docs/images.md, "Renderer"). Shared by the
 * helper (render/cctext-render.c) and the editor (core/img_svg.c).
 *
 * One helper process serves many requests, one at a time, over its
 * stdin / stdout. Integers are little-endian (both ends are the same
 * machine; the helper refuses to build big-endian). Every frame has a
 * fixed header; payload lengths are explicit so a reader can skip a
 * frame it does not want (a stale generation).
 *
 *   hello    (helper -> editor, once, after the sandbox is on)
 *            CrReply{type=HELLO, id=0, len=12} | u32 version | u32 kinds | u32 flags
 *   request  (editor -> helper)
 *            CrReq (52 bytes) | bytes[len]
 *   replies  (helper -> editor), for each request, in order:
 *            SIZE (w, h in CSS px) or ERROR; then, unless CR_F_SIZE_ONLY
 *            and after a SIZE, PIXELS (premultiplied RGBA8 at the device
 *            box) or ERROR.
 *
 * `id` is the editor's generation stamp: replies carry the id of their
 * request, and the editor drops any whose id is not the one it waits for
 * (a request it gave up on). A frame the helper cannot parse (bad magic,
 * a length over CR_INPUT_HARD_MAX) ends the process: the editor respawns.
 *
 * MERMAID (docs/images.md, "Mermaid"): the payload is one line of JSON
 * options, a newline, then the diagram source:
 *     {"theme":"dark","themeVariables":{...},"fontFamily":"..."}\n<source>
 * The helper runs mermaid.min.js in QuickJS (lazily started), rasterizes
 * the SVG it makes with lunasvg and answers like SVG (SIZE, then PIXELS
 * unless size-only). It keeps the last few diagrams' SVG by the payload's
 * hash, so a size-only request then a pixel request at the display box
 * run the script once. `node_max` refuses a diagram with more nodes
 * (CR_E_TOO_LARGE, "diagram too large to render (N nodes, limit M)");
 * `budget_ms` ends a script that runs longer (CR_E_TIMEOUT; the engine
 * restarts). A syntax error is CR_E_SCRIPT with mermaid's message.
 *
 * STATS answers one INFO frame (a line of JSON: engine starts, recycles,
 * heap, cache hits) for tests and benchmarks.
 *
 * MATH_TEX / MATH_MML (docs/images.md, "Math"): the payload is the
 * formula's source (TeX without delimiters, or a MathML <math> element).
 * The header carries the options: CR_F_DISPLAY (display style, else
 * inline), em_px (the font size the formula is set at), fg (0xRRGGBBAA:
 * what MathJax's currentColor becomes), max_w (CSS px a display formula
 * may take before MathJax breaks its lines; 0: none), node_max (refuse a
 * formula with more MathML nodes: CR_E_TOO_LARGE, "formula too large (N
 * nodes, limit M)") and budget_ms (the script's time; CR_E_TIMEOUT). The
 * helper runs MathJax 4 in QuickJS (its own engine, lazily started),
 * rasterizes the SVG with lunasvg and answers SIZE (baseline_css: the
 * baseline's distance from the bottom, so inline math sits on the text's
 * baseline), then PIXELS unless size-only. A formula that does not parse
 * is ERROR CR_E_SCRIPT with MathJax's message ("TeX: Missing close brace"),
 * never a picture of the error: the editor keeps the last good picture
 * up and says why. The last few formulas' SVG is kept by request key.
 *
 * A helper answers ERROR CR_E_KIND for a kind it does not carry (its
 * hello lists what it does).
 */
#ifndef CR_PROTO_H
#define CR_PROTO_H

#include <stdint.h>

#define CR_PROTO_VERSION 2u
#define CR_MAGIC_REQ 0x32515243u /* "CRQ2" (v1 requests were 44 bytes, "CRQ1") */
#define CR_MAGIC_REP 0x31535243u /* "CRS1" */

/* The helper's own ceilings; the editor's settings are lower. */
#define CR_INPUT_HARD_MAX (64u << 20)       /* request payload bytes */
#define CR_PIXELS_HARD_MAX (64u << 20)      /* output pixels (w * h), 256 MiB RGBA */
#define CR_SIDE_HARD_MAX 32768u             /* either output side */
/* SVG filter effects (lunasvg_set_filter_limits, render/cr_svg.cpp). */
#define CR_FILTER_PIXELS_MAX (16u << 20)    /* one filter region, device px; larger draws unfiltered */
#define CR_FILTER_BLUR_MAX 512.0f           /* blur standard deviation, device px (clamped) */
#define CR_FILTER_PRIMITIVES_MAX 64         /* primitives per filter (the rest are ignored) */
#define CR_FILTER_BYTES_MAX (512u << 20)    /* live intermediate filter images; past it unfiltered */

enum {
    CR_KIND_QUIT = 0,
    CR_KIND_SVG = 1,
    CR_KIND_TEX = 2,       /* math: TeX source (MathJax in QuickJS) */
    CR_KIND_MATHML = 3,    /* math: MathML source */
    CR_KIND_MERMAID = 4,   /* payload: options JSON line + source */
    CR_KIND_STATS = 5      /* answer INFO (JSON counters) */
};

enum {
    CR_F_DISPLAY = 1,      /* math: display style (else inline) */
    CR_F_DARK = 2,         /* unused (Mermaid's theme is in its options) */
    CR_F_SIZE_ONLY = 4     /* answer SIZE only, no pixels */
};

enum {
    CR_R_HELLO = 0,
    CR_R_SIZE = 1,
    CR_R_PIXELS = 2,
    CR_R_ERROR = 3,
    CR_R_INFO = 4          /* STATS: utf8 JSON */
};

/* Hello flags. */
enum {
    CR_H_SANDBOXED = 1,    /* the platform lockdown is on */
    CR_H_DISABLED = 2      /* this platform refuses to render (Windows stub) */
};

enum {
    CR_E_PARSE = 1,        /* not a document the engine reads */
    CR_E_EMPTY = 2,        /* zero or non-finite size */
    CR_E_PIXELS = 3,       /* output over the request's pixel cap */
    CR_E_NOMEM = 4,
    CR_E_KIND = 5,         /* a kind this helper does not carry */
    CR_E_DISABLED = 6,     /* the platform stub: no sandbox, no rendering */
    CR_E_TOO_LARGE = 7,    /* Mermaid / math: over node_max (message says how many) */
    CR_E_TIMEOUT = 8,      /* Mermaid / math: the script passed budget_ms */
    CR_E_SCRIPT = 9,       /* Mermaid / math: does not parse (the engine's message) */
    CR_E_ENGINE = 10       /* Mermaid / math: no engine (missing or damaged bytecode) */
};

/* 52 bytes, naturally aligned (no padding). */
typedef struct {
    uint32_t magic;        /* CR_MAGIC_REQ */
    uint32_t id;           /* generation stamp, echoed */
    uint8_t kind;          /* CR_KIND_* */
    uint8_t flags;         /* CR_F_* */
    uint16_t max_w;        /* math: display line-break width, CSS px (0: none) */
    float scale;           /* device px per CSS px, when box_w / box_h are 0 */
    float em_px;           /* math: font size, px (0: 16) */
    uint32_t fg;           /* math: 0xRRGGBBAA for currentColor (Mermaid: unused) */
    uint32_t bg;           /* 0xRRGGBBAA under the drawing; 0 = transparent */
    uint32_t box_w, box_h; /* output pixels exactly (both set), else scale */
    uint32_t max_px;       /* output pixel cap for this request (0 = hard max) */
    uint32_t len;          /* payload bytes that follow */
    uint32_t budget_ms;    /* Mermaid / math: script time budget (0: the helper's 10 s) */
    uint32_t node_max;     /* Mermaid / math: refuse larger inputs (0: no cap) */
} CrReq;

/* 16 bytes. */
typedef struct {
    uint32_t magic;        /* CR_MAGIC_REP */
    uint32_t id;
    uint32_t type;         /* CR_R_* */
    uint32_t len;          /* payload bytes that follow */
} CrReply;

/* SIZE payload (20 bytes). */
typedef struct {
    float w_css, h_css;    /* intrinsic size, CSS px */
    float baseline_css;    /* from the bottom (math; 0 for SVG) */
    uint32_t px_w, px_h;   /* what PIXELS will carry */
} CrSize;

/* PIXELS payload: u32 w | u32 h | u32 stride | RGBA8 premultiplied [h * stride].
 * ERROR payload: u32 code | utf8 message. */

typedef char cr_req_size_check[sizeof(CrReq) == 52 ? 1 : -1];
typedef char cr_rep_size_check[sizeof(CrReply) == 16 ? 1 : -1];
typedef char cr_size_size_check[sizeof(CrSize) == 20 ? 1 : -1];

#endif /* CR_PROTO_H */
