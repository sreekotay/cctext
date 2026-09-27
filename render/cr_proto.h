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
 *            CrReq (44 bytes) | bytes[len]
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
 * Kinds 2-4 (TeX, MathML, Mermaid) are reserved for the later steps of
 * the renderer plan: a helper answers ERROR CR_E_KIND for a kind it does
 * not carry (its hello lists what it does).
 */
#ifndef CR_PROTO_H
#define CR_PROTO_H

#include <stdint.h>

#define CR_PROTO_VERSION 1u
#define CR_MAGIC_REQ 0x31515243u /* "CRQ1" */
#define CR_MAGIC_REP 0x31535243u /* "CRS1" */

/* The helper's own ceilings; the editor's settings are lower. */
#define CR_INPUT_HARD_MAX (64u << 20)       /* request payload bytes */
#define CR_PIXELS_HARD_MAX (64u << 20)      /* output pixels (w * h), 256 MiB RGBA */
#define CR_SIDE_HARD_MAX 32768u             /* either output side */

enum {
    CR_KIND_QUIT = 0,
    CR_KIND_SVG = 1,
    CR_KIND_TEX = 2,       /* reserved: math (MathJax in QuickJS) */
    CR_KIND_MATHML = 3,    /* reserved */
    CR_KIND_MERMAID = 4    /* reserved */
};

enum {
    CR_F_DISPLAY = 1,      /* math: display style (reserved) */
    CR_F_DARK = 2,         /* mermaid: dark theme (reserved) */
    CR_F_SIZE_ONLY = 4     /* answer SIZE only, no pixels */
};

enum {
    CR_R_HELLO = 0,
    CR_R_SIZE = 1,
    CR_R_PIXELS = 2,
    CR_R_ERROR = 3
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
    CR_E_DISABLED = 6      /* the platform stub: no sandbox, no rendering */
};

/* 44 bytes, naturally aligned (no padding). */
typedef struct {
    uint32_t magic;        /* CR_MAGIC_REQ */
    uint32_t id;           /* generation stamp, echoed */
    uint8_t kind;          /* CR_KIND_* */
    uint8_t flags;         /* CR_F_* */
    uint16_t reserved;
    float scale;           /* device px per CSS px, when box_w / box_h are 0 */
    float em_px;           /* math: font size (reserved) */
    uint32_t fg;           /* 0xRRGGBBAA (reserved: math / mermaid) */
    uint32_t bg;           /* 0xRRGGBBAA under the drawing; 0 = transparent */
    uint32_t box_w, box_h; /* output pixels exactly (both set), else scale */
    uint32_t max_px;       /* output pixel cap for this request (0 = hard max) */
    uint32_t len;          /* payload bytes that follow */
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

typedef char cr_req_size_check[sizeof(CrReq) == 44 ? 1 : -1];
typedef char cr_rep_size_check[sizeof(CrReply) == 16 ? 1 : -1];
typedef char cr_size_size_check[sizeof(CrSize) == 20 ? 1 : -1];

#endif /* CR_PROTO_H */
