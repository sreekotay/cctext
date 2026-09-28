/*
 * cctext-render: the sandboxed renderer helper (docs/images.md,
 * "Renderer" and "Mermaid"). Step 1 of the renderer plan: SVG through
 * lunasvg; step 3: Mermaid through the official mermaid.min.js in QuickJS,
 * rasterized by lunasvg. The protocol and the pack hold the math step too.
 *
 *   cctext-render [--pack PATH] [--no-sandbox] [--recycle-mb N] [--recycle-jobs N]
 *                                                   serve requests on stdin / stdout
 *   cctext-render --build-pack MANIFEST OUT [HASH.h]
 *                                                   build step: fonts + JS bytecode -> pack
 *   cctext-render --png IN.svg OUT.png [SCALE]      development: render a file
 *   cctext-render --mermaid IN.mmd OUT.png|OUT.svg [dark] [SCALE]
 *                                                   development: a diagram (no sandbox)
 *   cctext-render --version
 *
 * Startup: close every fd above 2, read the pack, inflate and register the
 * fonts, warm the SVG engine once (lazy statics initialise before
 * lockdown), then lock down (sandbox_*.c) and say hello. After lockdown
 * the process reads stdin, writes stdout / stderr and allocates memory;
 * nothing else. The JavaScript bytecode stays compressed in the pack's
 * memory until the first Mermaid request: then its SHA-256 is checked
 * against the one compiled into this binary (CR_PACK_JS_HASHES, written
 * by the build step), it is inflated and QuickJS starts. The editor
 * (core/img_svg.c) enforces its own time budget on top of the engine's:
 * it kills and respawns a helper that overruns.
 *
 * Built with -DCR_SELFTEST (bin/cctext-render-selftest, tests only), the
 * environment variable CR_SELFTEST names an escape to attempt right after
 * lockdown (socket, exec, fork, mmapx, readfd, thread, open), and a
 * document (an SVG payload, or a Mermaid source) that starts with
 * "<!--cr-selftest:hang-->" / ":crash-->" / ":slow-->" / ":slowpx-->"
 * hangs, crashes, answers after 300 ms, or sends pixels after 1.5 s; a
 * Mermaid source "%%cr-selftest:jsloop" runs an endless script (the
 * engine's interrupt ends it) and "%%cr-selftest:jsheap N" allocates N
 * MiB in the engine. The release build has none of it. Built with
 * -DCR_BOOTSTRAP, it only builds packs (the first link, before the pack's
 * hashes exist).
 */
#if !defined(_WIN32)
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#define cr_read _read
#define cr_write _write
#else
#include <unistd.h>
#define cr_read read
#define cr_write write
#endif
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#if defined(__linux__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#endif

#include "cr_js.h"
#include "cr_pack.h"
#include "cr_proto.h"
#include "cr_sha256.h"
#include "cr_svg.h"

#if !defined(CR_BOOTSTRAP)
#include "cr_pack_hash.h" /* out/render: CR_PACK_JS_HASHES */
#else
#define CR_PACK_JS_HASHES {0, 0}
#endif

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "cctext-render: the wire protocol is little-endian"
#endif

int cr_sandbox_lockdown(size_t mem_limit, unsigned cpu_seconds);

#define CR_VERSION                                                                               \
    "cctext-render 2 (lunasvg 3.5.0 cf3594d + cctext patches, plutovg 1.3.3, QuickJS a38171d, " \
    "mermaid 12.0.0)"
/* Address space and CPU seconds for the whole process life. The editor's
 * per-request time budget is far tighter; RLIMIT_CPU only ends a helper
 * that has worked for this long in total (the editor respawns it). */
#define CR_MEM_LIMIT ((size_t)2 << 30)
#define CR_CPU_SECONDS 600u
/* Mermaid defaults (the editor's settings send their own). */
#define CR_MM_BUDGET_MS 10000u
#define CR_MM_HEAP_MAX ((size_t)1 << 30)
#define CR_MM_ID "cctext-mermaid"

static int g_disabled;   /* no lockdown on this platform: refuse everything */

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

/* ---- I/O -------------------------------------------------------------- */

static int read_full(void *p, size_t n)
{
    uint8_t *b = p;
    while (n) {
        long r = (long)cr_read(0, b, (unsigned)(n > (1u << 30) ? (1u << 30) : n));
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return -1;
        }
        b += r;
        n -= (size_t)r;
    }
    return 0;
}

static int write_full(const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n) {
        long r = (long)cr_write(1, b, (unsigned)(n > (1u << 30) ? (1u << 30) : n));
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return -1;
        }
        b += r;
        n -= (size_t)r;
    }
    return 0;
}

static void send_reply(uint32_t id, uint32_t type, const void *a, size_t an, const void *b,
                       size_t bn)
{
    CrReply h = {CR_MAGIC_REP, id, type, (uint32_t)(an + bn)};
    if (write_full(&h, sizeof h) || (an && write_full(a, an)) || (bn && write_full(b, bn)))
        exit(0); /* the editor went away */
}

static void send_error(uint32_t id, uint32_t code, const char *msg)
{
    send_reply(id, CR_R_ERROR, &code, 4, msg, strlen(msg));
}

/* ---- the pack ----------------------------------------------------------- */

static CrPack g_pack;

static uint8_t *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    long len;
    uint8_t *b;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0 ||
        len > (256l << 20)) {
        fclose(f);
        return NULL;
    }
    b = malloc((size_t)len + 1);
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) {
        free(b);
        b = NULL;
    }
    fclose(f);
    if (b) {
        b[len] = 0;
        *n = (size_t)len;
    }
    return b;
}

/* <dir of this executable>/cctext-render.pack */
static void default_pack(char *out, size_t n)
{
    char exe[3072];
    size_t len = 0;
    snprintf(out, n, "cctext-render.pack");
#if defined(__linux__)
    {
        long r = (long)readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (r <= 0) return;
        len = (size_t)r;
        exe[len] = 0;
    }
#elif defined(__APPLE__)
    {
        uint32_t sz = sizeof exe;
        if (_NSGetExecutablePath(exe, &sz) != 0) return;
        len = strlen(exe);
    }
#else
    (void)exe;
    return;
#endif
    while (len && exe[len - 1] != '/') len--;
    if (!len) return;
    exe[len] = 0;
    snprintf(out, n, "%scctext-render.pack", exe);
}

/* Entries "font:<family>:<bold>:<italic>". */
static int fonts_init(void)
{
    int nf = 0;
    for (int i = 0; i < g_pack.n; i++) {
        CrAsset *a = &g_pack.e[i];
        char fam[256];
        int bold = 0, italic = 0;
        const char *p, *c1;
        const uint8_t *d;
        if (strncmp(a->name, "font:", 5) != 0) continue;
        p = a->name + 5;
        c1 = strchr(p, ':');
        if (!c1 || (size_t)(c1 - p) >= sizeof fam) continue;
        memcpy(fam, p, (size_t)(c1 - p));
        fam[c1 - p] = 0;
        if (sscanf(c1 + 1, "%d:%d", &bold, &italic) != 2) continue;
        d = cr_asset_data(a);
        if (d && cr_svg_add_font(fam, bold, italic, d, a->raw_len) == 0) nf++;
    }
    return nf;
}

/* Parse + draw once before lockdown: font-cache sorting and any other lazy
 * first-use work happens while the process may still do anything. */
static void warm_up(void)
{
    static const char svg[] =
        "<svg xmlns='http://www.w3.org/2000/svg' width='8' height='8'>"
        "<rect width='8' height='8' fill='#888' stroke='#000' stroke-dasharray='1'/>"
        "<text x='1' y='7' font-family='serif' font-weight='bold' font-size='6'>A</text>"
        "<filter id='f'><feGaussianBlur stdDeviation='1'/></filter>"
        "<circle cx='4' cy='4' r='2' filter='url(#f)'/>"
        "</svg>";
    float w = 0, h = 0;
    uint8_t px[8 * 8 * 4];
    cr_svg *d = cr_svg_parse(svg, sizeof svg - 1, &w, &h);
    if (d) {
        cr_svg_render(d, 1, 1, 8, 8, 0, px);
        cr_svg_free(d);
    }
    (void)cr_svg_measure("A", 1, 16, 400, 0, "sans-serif");
}

/* ---- JavaScript engines ------------------------------------------------- */

typedef struct {
    const char *name;
    const char *hex;
} CrJsHash;

static const CrJsHash g_js_hashes[] = {CR_PACK_JS_HASHES};

/* The SHA-256 compiled into this binary for pack entry `name`, or NULL. */
static const char *js_hash_of(const char *name)
{
    for (size_t i = 0; i < sizeof g_js_hashes / sizeof g_js_hashes[0]; i++)
        if (g_js_hashes[i].name && strcmp(g_js_hashes[i].name, name) == 0) return g_js_hashes[i].hex;
    return NULL;
}

static CrJsPolicy g_policy = {CR_MM_HEAP_MAX, (size_t)256 << 20, 0, (size_t)16 << 20};
static CrJs *g_mermaid;
static CrAsset *g_mermaid_asset;
static int g_mermaid_bad;        /* the bytecode failed its check: never try again */
static double g_inflate_ms;

/* The Mermaid engine, created on the first request (the bytecode checked
 * and inflated here, from the pack read before lockdown). NULL + message
 * when there is none. */
static CrJs *mermaid_engine(char *err, size_t cap)
{
    const char *want;
    char got[65];
    const uint8_t *bc;
    double t0;
    if (g_mermaid) return g_mermaid;
    if (g_mermaid_bad || !g_mermaid_asset) {
        snprintf(err, cap, "no Mermaid engine in this renderer");
        return NULL;
    }
    want = js_hash_of("js:mermaid");
    cr_sha256_hex(g_mermaid_asset->comp, g_mermaid_asset->comp_len, got);
    if (!want || strcmp(want, got) != 0) {
        g_mermaid_bad = 1;
        fprintf(stderr, "cctext-render: js:mermaid in the pack is not the one this build made\n");
        snprintf(err, cap, "the Mermaid engine does not match this renderer (rebuild the pack)");
        return NULL;
    }
    t0 = now_ms();
    bc = cr_asset_data(g_mermaid_asset); /* inflated once, kept for restarts */
    g_inflate_ms = now_ms() - t0;
    if (!bc) {
        g_mermaid_bad = 1;
        snprintf(err, cap, "the Mermaid engine is damaged");
        return NULL;
    }
    g_mermaid = cr_js_new("mermaid", bc, g_mermaid_asset->raw_len, &g_policy);
    if (!g_mermaid) snprintf(err, cap, "out of memory");
    return g_mermaid;
}

/* ---- the math engine (docs/images.md, "Math") ---------------------------
 * MathJax 4 in its own QuickJS engine: the bytecode (js:math) is checked
 * and inflated like Mermaid's, lazily; the TeX extensions and dynamic font
 * files a formula asks for come from src: entries, each checked against
 * its SHA-256 (compiled into this binary) before it is evaluated, then
 * inflated, evaluated and freed (the realm keeps what it defined). */
static CrJs *g_math;
static CrAsset *g_math_asset;
static int g_math_bad;
static double g_math_inflate_ms;
static unsigned g_math_loads;    /* source modules evaluated */
static uint8_t *g_src_ok;        /* per pack entry: its hash was checked */

static int math_src_get(void *ctx, const char *key, const char **src, size_t *n, char *err,
                        size_t cap)
{
    char name[300], got[65];
    const char *want;
    CrAsset *a;
    int idx;
    (void)ctx;
    if (snprintf(name, sizeof name, "src:%s", key) >= (int)sizeof name) return 1;
    a = cr_pack_find(&g_pack, name);
    if (!a) return 1; /* no such module: __hostLoad answers false */
    idx = (int)(a - g_pack.e);
    if (!g_src_ok) g_src_ok = calloc((size_t)g_pack.n + 1, 1);
    if (!g_src_ok) {
        snprintf(err, cap, "out of memory");
        return 1;
    }
    if (!g_src_ok[idx]) {
        want = js_hash_of(name);
        cr_sha256_hex(a->comp, a->comp_len, got);
        if (!want || strcmp(want, got) != 0) {
            fprintf(stderr, "cctext-render: %s in the pack is not the one this build made\n", name);
            snprintf(err, cap, "%s does not match this renderer (rebuild the pack)", key);
            return 1;
        }
        g_src_ok[idx] = 1;
    }
    *src = (const char *)cr_asset_data(a);
    if (!*src) {
        snprintf(err, cap, "%s is damaged", key);
        return 1;
    }
    *n = a->raw_len;
    g_math_loads++;
    return 0;
}

static void math_src_put(void *ctx, const char *key)
{
    char name[300];
    (void)ctx;
    snprintf(name, sizeof name, "src:%s", key);
    cr_asset_drop(cr_pack_find(&g_pack, name)); /* the realm holds what it defined */
}

static CrJs *math_engine(char *err, size_t cap)
{
    const char *want;
    char got[65];
    const uint8_t *bc;
    double t0;
    if (g_math) return g_math;
    if (g_math_bad || !g_math_asset) {
        snprintf(err, cap, "no math engine in this renderer");
        return NULL;
    }
    want = js_hash_of("js:math");
    cr_sha256_hex(g_math_asset->comp, g_math_asset->comp_len, got);
    if (!want || strcmp(want, got) != 0) {
        g_math_bad = 1;
        fprintf(stderr, "cctext-render: js:math in the pack is not the one this build made\n");
        snprintf(err, cap, "the math engine does not match this renderer (rebuild the pack)");
        return NULL;
    }
    t0 = now_ms();
    bc = cr_asset_data(g_math_asset); /* inflated once, kept for restarts */
    g_math_inflate_ms = now_ms() - t0;
    if (!bc) {
        g_math_bad = 1;
        snprintf(err, cap, "the math engine is damaged");
        return NULL;
    }
    g_math = cr_js_new("math", bc, g_math_asset->raw_len, &g_policy);
    if (!g_math) {
        snprintf(err, cap, "out of memory");
        return NULL;
    }
    cr_js_set_loader(g_math, math_src_get, math_src_put, NULL);
    return g_math;
}

/* ---- Mermaid SVG cache ---------------------------------------------------
 * The last few diagrams' SVG by the payload's hash: a size-only request
 * then the pixel request at the display box run the script once. */
#define MM_CACHE 4
typedef struct {
    uint64_t h;
    size_t n;
    char *svg;
    size_t svg_n;
    double t;
} MmEnt;
static MmEnt g_mm[MM_CACHE];
static unsigned g_mm_hits, g_mm_runs;

static uint64_t fnv64(const void *p, size_t n)
{
    const uint8_t *s = p;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) {
        h ^= s[i];
        h *= 1099511628211ull;
    }
    return h;
}

static MmEnt *mm_find(uint64_t h, size_t n)
{
    for (int i = 0; i < MM_CACHE; i++)
        if (g_mm[i].svg && g_mm[i].h == h && g_mm[i].n == n) return &g_mm[i];
    return NULL;
}

static void mm_put(uint64_t h, size_t n, char *svg, size_t svg_n)
{
    MmEnt *v = &g_mm[0];
    for (int i = 1; i < MM_CACHE; i++)
        if (!g_mm[i].svg || (v->svg && g_mm[i].t < v->t)) v = &g_mm[i];
    free(v->svg);
    v->h = h;
    v->n = n;
    v->svg = svg;
    v->svg_n = svg_n;
    v->t = now_ms();
}

/* ---- requests ----------------------------------------------------------- */

#ifdef CR_SELFTEST
static void selftest_spin(long ms)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    do clock_gettime(CLOCK_MONOTONIC, &t1);
    while ((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000 < ms);
}

/* ":slowpx-->" delays only pixel requests (1.5 s): a size arrives, then
 * the pixels much later (ui_svg_test: layout stable across the gap). */
static void selftest_payload(const char *src, size_t n, int size_only)
{
    static const char pre[] = "<!--cr-selftest:";
    if (n < sizeof pre - 1 || memcmp(src, pre, sizeof pre - 1) != 0) return;
    src += sizeof pre - 1;
    if (strncmp(src, "hang-->", 7) == 0)
        for (volatile unsigned long i = 0;; i++) {
        }
    if (strncmp(src, "crash-->", 8) == 0) abort();
    if (strncmp(src, "slow-->", 7) == 0) selftest_spin(300);
    if (strncmp(src, "slowpx-->", 9) == 0 && !size_only) selftest_spin(1500);
}
#endif

/* SVG text -> SIZE (+ PIXELS) at the request's box or scale. */
static void emit_svg_b(const CrReq *q, const char *src, size_t n, float baseline)
{
    float w = 0, h = 0, sx, sy;
    uint64_t cap = q->max_px && q->max_px < CR_PIXELS_HARD_MAX ? q->max_px : CR_PIXELS_HARD_MAX;
    uint32_t pw, ph;
    CrSize sz;
    cr_svg *doc = cr_svg_parse(src, n, &w, &h);
    if (!doc) {
        send_error(q->id, CR_E_PARSE, "not an SVG document this renderer reads");
        return;
    }
    if (!(w > 0) || !(h > 0) || !isfinite(w) || !isfinite(h) || w > 1e7f || h > 1e7f) {
        cr_svg_free(doc);
        send_error(q->id, CR_E_EMPTY, "no size (width / height / viewBox)");
        return;
    }
    if (q->box_w && q->box_h) {
        pw = q->box_w;
        ph = q->box_h;
        sx = (float)pw / w;
        sy = (float)ph / h;
        /* The natural size rounded up (a diagram's CSS size is fractional):
         * draw at 1:1, not stretched by a fraction of a pixel (every edge
         * would blur). */
        if (pw == (uint32_t)ceil((double)w) && ph == (uint32_t)ceil((double)h)) sx = sy = 1.0f;
    } else {
        float s = q->scale > 0 && q->scale <= 64 && isfinite(q->scale) ? q->scale : 1.0f;
        double fw = ceil((double)w * s), fh = ceil((double)h * s);
        sx = sy = s;
        pw = fw < 1 ? 1 : fw > 4e9 ? 0xffffffffu : (uint32_t)fw;
        ph = fh < 1 ? 1 : fh > 4e9 ? 0xffffffffu : (uint32_t)fh;
    }
    sz.w_css = w;
    sz.h_css = h;
    sz.baseline_css = baseline >= 0 && isfinite(baseline) ? baseline : 0;
    sz.px_w = pw;
    sz.px_h = ph;
    if (q->flags & CR_F_SIZE_ONLY) {
        cr_svg_free(doc);
        send_reply(q->id, CR_R_SIZE, &sz, sizeof sz, NULL, 0);
        return;
    }
    if (pw > CR_SIDE_HARD_MAX || ph > CR_SIDE_HARD_MAX || (uint64_t)pw * ph > cap) {
        cr_svg_free(doc);
        send_reply(q->id, CR_R_SIZE, &sz, sizeof sz, NULL, 0);
        send_error(q->id, CR_E_PIXELS, "output over the pixel limit");
        return;
    }
    send_reply(q->id, CR_R_SIZE, &sz, sizeof sz, NULL, 0);
    {
        size_t nb = (size_t)pw * ph * 4;
        uint8_t *buf = malloc(nb);
        uint32_t hdr[3] = {pw, ph, pw * 4};
        if (!buf) {
            cr_svg_free(doc);
            send_error(q->id, CR_E_NOMEM, "out of memory");
            return;
        }
        cr_svg_render(doc, sx, sy, pw, ph, q->bg, buf);
        cr_svg_free(doc);
        send_reply(q->id, CR_R_PIXELS, hdr, sizeof hdr, buf, nb);
        free(buf);
    }
}

/* Mermaid's error text as one line: its first line ("Parse error on line
 * 3:") and, for a parse error, the last ("Expecting 'SQE', got 'PE'"); the
 * source excerpt and the caret line between them are dropped. */
static void one_line_error(const char *err, char *out, size_t cap)
{
    const char *p = err, *e, *last = NULL;
    size_t k;
    if (!strncmp(p, "Error: ", 7)) p += 7;
    e = p + strcspn(p, "\n");
    for (const char *l = *e ? e + 1 : e; *l;) {
        if (strncmp(l, "Expecting", 9) == 0) last = l;
        l += strcspn(l, "\n");
        if (*l) l++;
    }
    k = (size_t)(e - p);
    if (!k) {
        snprintf(out, cap, "diagram does not parse");
        return;
    }
    if (last)
        snprintf(out, cap, "%.*s %.*s", (int)k, p, (int)strcspn(last, "\n"), last);
    else
        snprintf(out, cap, "%.*s", (int)k, p);
}

/* Mermaid: payload "<options JSON>\n<source>". The engine starts (lazily)
 * before anything of the payload is looked at. */
static void handle_mermaid(const CrReq *q, char *payload)
{
    char err[1024], opts[4096 + 64];
    const char *argv[3];
    const char *nl, *src;
    size_t on, n = q->len;
    uint64_t h = fnv64(payload, n);
    MmEnt *hit;
    char *svg;
    int timed_out = 0;
    CrJs *e = mermaid_engine(err, sizeof err);
    if (!e) {
        send_error(q->id, CR_E_ENGINE, err);
        return;
    }
    hit = mm_find(h, n);
    if (hit) {
        g_mm_hits++;
        hit->t = now_ms();
        emit_svg_b(q, hit->svg, hit->svg_n, 0);
        return;
    }
    nl = memchr(payload, '\n', n);
    if (!nl || (size_t)(nl - payload) > 4096) {
        send_error(q->id, CR_E_PARSE, "no options line");
        return;
    }
    src = nl + 1;
#ifdef CR_SELFTEST
    selftest_payload(src, n - (size_t)(src - payload), (q->flags & CR_F_SIZE_ONLY) != 0);
    if (strncmp(src, "%%cr-selftest:jsloop", 20) == 0) src = "%%cr-selftest:jsloop";
#endif
    /* The options line is spliced into a JSON object with the per-request
     * limits (the editor wrote it; JSON.parse in the engine checks it). */
    on = (size_t)(nl - payload);
    if (on < 2 || payload[0] != '{' || payload[on - 1] != '}') {
        send_error(q->id, CR_E_PARSE, "bad options line");
        return;
    }
    snprintf(opts, sizeof opts, "{\"nodeMax\":%u,\"maxTextSize\":%u%s%.*s", q->node_max,
             CR_INPUT_HARD_MAX, on > 2 ? "," : "}", on > 2 ? (int)(on - 1) : 0, payload + 1);
    argv[0] = CR_MM_ID;
    argv[1] = src;
    argv[2] = opts;
    g_mm_runs++;
#ifdef CR_SELFTEST
    if (strcmp(src, "%%cr-selftest:jsloop") == 0) {
        static const char *loop[] = {"0"};
        svg = cr_js_call(e, "__cctextSelftestLoop", 1, loop, q->budget_ms ? q->budget_ms : CR_MM_BUDGET_MS,
                         err, sizeof err, &timed_out);
    } else if (strncmp(src, "%%cr-selftest:jsheap ", 21) == 0) {
        const char *mb[] = {src + 21};
        svg = cr_js_call(e, "__cctextSelftestHeap", 1, mb, q->budget_ms ? q->budget_ms : CR_MM_BUDGET_MS,
                         err, sizeof err, &timed_out);
    } else
#endif
    svg = cr_js_call(e, "mmRender", 3, argv, q->budget_ms ? q->budget_ms : CR_MM_BUDGET_MS, err,
                     sizeof err, &timed_out);
    if (svg && strncmp(svg, "CCTEXT_TOO_LARGE ", 17) == 0) {
        unsigned cnt = 0, lim = 0;
        char m[160];
        if (sscanf(svg + 17, "%u %u", &cnt, &lim) != 2) cnt = lim = 0;
        snprintf(m, sizeof m, "diagram too large to render (%u nodes, limit %u)", cnt, lim);
        free(svg);
        send_error(q->id, CR_E_TOO_LARGE, m);
        return;
    }
    if (svg && strncmp(svg, "CCTEXT_PARSE ", 13) == 0) {
        char m[400];
        one_line_error(svg + 13, m, sizeof m);
        free(svg);
        send_error(q->id, CR_E_SCRIPT, m);
        return;
    }
    if (!svg) {
        if (timed_out) {
            send_error(q->id, CR_E_TIMEOUT, "diagram took too long to render");
        } else {
            char m[400];
            one_line_error(err, m, sizeof m);
            send_error(q->id, CR_E_SCRIPT, m);
        }
        return;
    }
    mm_put(h, n, svg, strlen(svg));
    emit_svg_b(q, svg, strlen(svg), 0);
}

/* ---- math (docs/images.md, "Math") --------------------------------------- */

static unsigned g_math_runs, g_math_hits;

/* One line of printable text (the editor paints it). */
static void math_msg(const char *s, char *out, size_t cap)
{
    size_t k = 0;
    if (!strncmp(s, "Error: ", 7)) s += 7;
    for (; *s && k + 1 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        out[k++] = c < 0x20 || c == 0x7f ? ' ' : (char)c;
    }
    out[k] = 0;
    if (!k) snprintf(out, cap, "formula does not parse");
}

/* An mjRender result "{"w":..,"h":..,"b":..}\n<svg...>" -> SIZE + PIXELS. */
static void emit_math(const CrReq *q, const char *res)
{
    const char *nl = strchr(res, '\n'), *p;
    float b = 0;
    if (!nl) {
        send_error(q->id, CR_E_PARSE, "no math output");
        return;
    }
    p = strstr(res, "\"b\":");
    if (p && p < nl) b = (float)atof(p + 4);
    emit_svg_b(q, nl + 1, strlen(nl + 1), b);
}

/* TeX or MathML source -> SIZE (with the baseline) + PIXELS. Options come
 * in the request header (cr_proto.h); the key of the SVG cache is the
 * options and the source. */
static void handle_math(const CrReq *q, char *src)
{
    char err[1024], opts[256], fgs[8];
    const char *argv[3];
    uint64_t h;
    size_t n = q->len;
    MmEnt *hit;
    char *res;
    int timed_out = 0;
    float em = q->em_px > 0 && q->em_px < 1000 && isfinite(q->em_px) ? q->em_px : 16.0f;
    CrJs *e = math_engine(err, sizeof err);
    if (!e) {
        send_error(q->id, CR_E_ENGINE, err);
        return;
    }
    snprintf(fgs, sizeof fgs, "#%06x", (unsigned)(q->fg >> 8) & 0xffffffu);
    snprintf(opts, sizeof opts,
             "{\"display\":%s,\"em\":%.3f,\"fg\":\"%s\",\"maxWidth\":%u,\"nodeMax\":%u}",
             (q->flags & CR_F_DISPLAY) ? "true" : "false", (double)em, fgs, (unsigned)q->max_w,
             q->node_max);
    h = fnv64(opts, strlen(opts)) ^ ((uint64_t)q->kind << 56);
    h = (h ^ fnv64(src, n)) * 1099511628211ull;
    hit = mm_find(h, n);
    if (hit) {
        g_math_hits++;
        hit->t = now_ms();
        emit_math(q, hit->svg);
        return;
    }
#ifdef CR_SELFTEST
    selftest_payload(src, n, (q->flags & CR_F_SIZE_ONLY) != 0);
#endif
    argv[0] = q->kind == CR_KIND_MATHML ? "mml" : "tex";
    argv[1] = src;
    argv[2] = opts;
    g_math_runs++;
#ifdef CR_SELFTEST
    if (strncmp(src, "%cr-selftest:jsloop", 19) == 0) {
        static const char *loop[] = {"0"};
        res = cr_js_call(e, "__cctextSelftestLoop", 1, loop,
                         q->budget_ms ? q->budget_ms : CR_MM_BUDGET_MS, err, sizeof err, &timed_out);
    } else if (strncmp(src, "%cr-selftest:jsheap ", 20) == 0) {
        const char *mb[] = {src + 20};
        res = cr_js_call(e, "__cctextSelftestHeap", 1, mb,
                         q->budget_ms ? q->budget_ms : CR_MM_BUDGET_MS, err, sizeof err, &timed_out);
    } else
#endif
    res = cr_js_call(e, "mjRender", 3, argv, q->budget_ms ? q->budget_ms : CR_MM_BUDGET_MS, err,
                     sizeof err, &timed_out);
    if (res && strncmp(res, "CCTEXT_TOO_LARGE ", 17) == 0) {
        unsigned cnt = 0, lim = 0;
        char m[160];
        if (sscanf(res + 17, "%u %u", &cnt, &lim) != 2) cnt = lim = 0;
        snprintf(m, sizeof m, "formula too large (%u nodes, limit %u)", cnt, lim);
        free(res);
        send_error(q->id, CR_E_TOO_LARGE, m);
        return;
    }
    if (res && strncmp(res, "CCTEXT_ERR ", 11) == 0) {
        char m[400];
        math_msg(res + 11, m, sizeof m);
        free(res);
        send_error(q->id, CR_E_SCRIPT, m);
        return;
    }
    if (!res) {
        char m[400];
        if (timed_out) {
            send_error(q->id, CR_E_TIMEOUT, "formula took too long to render");
        } else if (strstr(err, "stack overflow")) {
            send_error(q->id, CR_E_SCRIPT, "formula too deeply nested");
        } else if (strstr(err, "out of memory")) {
            send_error(q->id, CR_E_SCRIPT, "formula needs too much memory");
        } else {
            math_msg(err, m, sizeof m);
            send_error(q->id, CR_E_SCRIPT, m);
        }
        return;
    }
    mm_put(h, n, res, strlen(res));
    emit_math(q, res);
}

static void handle_stats(const CrReq *q)
{
    char b[1024];
    CrJsStats st, ms;
    memset(&st, 0, sizeof st);
    memset(&ms, 0, sizeof ms);
    if (g_mermaid) cr_js_stats(g_mermaid, &st);
    if (g_math) cr_js_stats(g_math, &ms);
    snprintf(b, sizeof b,
             "{\"engine\":%d,\"starts\":%u,\"recycles\":%u,\"jobs\":%u,\"total_jobs\":%u,"
             "\"gcs\":%u,\"heap\":%zu,\"start_ms\":%.1f,\"inflate_ms\":%.1f,\"gc_ms\":%.1f,"
             "\"cache_hits\":%u,\"runs\":%u,\"recycle_mb\":%zu,\"recycle_jobs\":%u,"
             "\"math_engine\":%d,\"math_starts\":%u,\"math_recycles\":%u,\"math_jobs\":%u,"
             "\"math_total_jobs\":%u,\"math_heap\":%zu,\"math_start_ms\":%.1f,"
             "\"math_inflate_ms\":%.1f,\"math_loads\":%u,\"math_runs\":%u,\"math_hits\":%u}",
             g_mermaid != NULL, st.starts, st.recycles, st.jobs, st.total_jobs, st.gcs, st.heap,
             st.last_start_ms, g_inflate_ms, st.gc_ms, g_mm_hits, g_mm_runs,
             g_policy.recycle_heap >> 20, g_policy.recycle_jobs, g_math != NULL, ms.starts,
             ms.recycles, ms.jobs, ms.total_jobs, ms.heap, ms.last_start_ms, g_math_inflate_ms,
             g_math_loads, g_math_runs, g_math_hits);
    send_reply(q->id, CR_R_INFO, b, strlen(b), NULL, 0);
}

static void serve(int sandboxed)
{
    uint32_t kinds = g_disabled ? 0u : (1u << CR_KIND_SVG);
    uint32_t hello[3];
    if (!g_disabled && g_mermaid_asset && js_hash_of("js:mermaid")) kinds |= 1u << CR_KIND_MERMAID;
    if (!g_disabled && g_math_asset && js_hash_of("js:math"))
        kinds |= (1u << CR_KIND_TEX) | (1u << CR_KIND_MATHML);
    hello[0] = CR_PROTO_VERSION;
    hello[1] = kinds;
    hello[2] = (sandboxed ? CR_H_SANDBOXED : 0u) | (g_disabled ? CR_H_DISABLED : 0u);
    send_reply(0, CR_R_HELLO, hello, sizeof hello, NULL, 0);
    for (;;) {
        CrReq q;
        char *src;
        if (read_full(&q, sizeof q) != 0) exit(0);
        if (q.magic != CR_MAGIC_REQ) {
            fprintf(stderr, "cctext-render: bad frame magic 0x%08x; exiting\n", q.magic);
            exit(3);
        }
        if (q.kind == CR_KIND_QUIT) exit(0);
        if (q.len > CR_INPUT_HARD_MAX) {
            fprintf(stderr, "cctext-render: frame of %u bytes over the %u limit; exiting\n", q.len,
                    CR_INPUT_HARD_MAX);
            exit(3);
        }
        src = malloc((size_t)q.len + 1);
        if (!src) exit(4);
        if (read_full(src, q.len) != 0) exit(0);
        src[q.len] = 0;
#ifdef CR_SELFTEST
        if (q.kind == CR_KIND_SVG) selftest_payload(src, q.len, (q.flags & CR_F_SIZE_ONLY) != 0);
#endif
        if (q.kind == CR_KIND_STATS)
            handle_stats(&q);
        else if (g_disabled)
            send_error(q.id, CR_E_DISABLED, "renderer disabled on this platform (no sandbox)");
        else if (q.kind == CR_KIND_SVG)
            emit_svg_b(&q, src, q.len, 0);
        else if (q.kind == CR_KIND_MERMAID && (kinds & (1u << CR_KIND_MERMAID)))
            handle_mermaid(&q, src);
        else if ((q.kind == CR_KIND_TEX || q.kind == CR_KIND_MATHML) && (kinds & (1u << q.kind)))
            handle_math(&q, src);
        else
            send_error(q.id, CR_E_KIND, "kind not in this renderer");
        free(src);
    }
}

#ifdef CR_SELFTEST
#include <sys/mman.h>
#include <sys/socket.h>
static int selftest(const char *t)
{
    /* The policy: a file open fails soft, then the named escape must kill
     * the process (SIGSYS) before "survived" prints. Seatbelt (macOS)
     * refuses with an error instead: exit 0 when the call failed, 1 when
     * it went through. It cannot refuse a thread ("thread" survives). */
    FILE *f = fopen("/etc/passwd", "r");
    int esc = -1;
    fprintf(stderr, "[selftest] fopen(/etc/passwd) -> %s (errno %d)\n", f ? "OPENED" : "denied",
            f ? 0 : errno);
#if defined(__linux__)
    if (!strcmp(t, "open")) return f ? 1 : (errno == EACCES ? 0 : 2);
    if (!strcmp(t, "socket")) esc = syscall(SYS_socket, 2, 1, 0) >= 0;
#else
    /* the rlimit on descriptors may refuse the open before the profile */
    if (!strcmp(t, "open"))
        return f ? 1 : (errno == EACCES || errno == EPERM || errno == EMFILE ? 0 : 2);
    if (!strcmp(t, "socket")) esc = socket(AF_INET, SOCK_STREAM, 0) >= 0;
#endif
    if (!strcmp(t, "exec")) {
        char *a[] = {"/bin/true", NULL};
        execve(a[0], a, NULL);
        esc = 0;
    }
    if (!strcmp(t, "fork")) {
        pid_t p = fork();
        if (p == 0) _exit(0);
        esc = p > 0;
    }
    if (!strcmp(t, "mmapx"))
        esc = mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1,
                   0) != MAP_FAILED;
    if (!strcmp(t, "readfd")) {
        char c;
        esc = read(3, &c, 1) >= 0;
        if (!esc) fprintf(stderr, "[selftest] read(3) failed\n");
    }
#if defined(__linux__)
    if (!strcmp(t, "thread")) syscall(SYS_clone, 0x50f00, 0, 0, 0, 0);
#else
    if (esc >= 0) {
        fprintf(stderr, "[selftest] %s %s\n", t, esc ? "ESCAPED" : "denied");
        return esc ? 1 : 0;
    }
#endif
    fprintf(stderr, "[selftest] %s survived\n", t);
    return 9;
}
#endif

static void close_extra_fds(void)
{
#if defined(__linux__)
#ifdef SYS_close_range
    if (syscall(SYS_close_range, 3u, ~0u, 0u) == 0) return;
#endif
    for (int fd = 3; fd < 1024; fd++) (void)close(fd);
#elif defined(__APPLE__)
    for (int fd = 3; fd < 1024; fd++) (void)close(fd);
#endif
}

/* The pack, mapped read-only before lockdown (POSIX): pages come in as
 * they are used, so a helper holds the fonts and its own bundle, not the
 * whole 11 MB (cr_pack.h). Elsewhere, read whole. Kept: assets point into
 * it. */
static uint8_t *map_pack(const char *path, size_t *n)
{
#if defined(__linux__) || defined(__APPLE__)
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    void *m;
    if (fd < 0) return NULL;
    if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > (256l << 20)) {
        close(fd);
        return NULL;
    }
    m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return NULL;
    *n = (size_t)st.st_size;
    return m;
#else
    return slurp(path, n);
#endif
}

static int open_pack(const char *pack)
{
    size_t pn = 0;
    uint8_t *pb = map_pack(pack, &pn); /* kept: assets point into it */
    if (!pb || cr_pack_open(&g_pack, pb, pn) != 0) {
        fprintf(stderr, "cctext-render: cannot read pack %s\n", pack);
        return -1;
    }
    g_mermaid_asset = cr_pack_find(&g_pack, "js:mermaid");
    g_math_asset = cr_pack_find(&g_pack, "js:math");
    return 0;
}

/* --mermaid IN OUT [dark] [SCALE]: development, no sandbox. */
static int dev_mermaid(const char *pack, int argc, char **argv, int i)
{
    const char *in = argv[i + 1], *out = argv[i + 2];
    int dark = 0;
    float scale = 1;
    size_t n = 0;
    uint8_t *src;
    char err[1024], opts[256];
    const char *a[3];
    char *svg;
    int to = 0;
    double t0;
    CrJs *e;
    for (int k = i + 3; k < argc; k++) {
        if (!strcmp(argv[k], "dark")) dark = 1;
        else if (atof(argv[k]) > 0) scale = (float)atof(argv[k]);
    }
    if (open_pack(pack) != 0 || fonts_init() < 1) return 2;
    src = slurp(in, &n);
    if (!src) {
        fprintf(stderr, "cctext-render: cannot read %s\n", in);
        return 2;
    }
    e = mermaid_engine(err, sizeof err);
    if (!e) {
        fprintf(stderr, "cctext-render: %s\n", err);
        return 2;
    }
    snprintf(opts, sizeof opts, "{\"theme\":\"%s\"}", dark ? "dark" : "default");
    a[0] = CR_MM_ID;
    a[1] = (const char *)src;
    a[2] = opts;
    t0 = now_ms();
    svg = cr_js_call(e, "mmRender", 3, a, 60000, err, sizeof err, &to);
    if (!svg) {
        fprintf(stderr, "cctext-render: %s\n", err);
        return 1;
    }
    fprintf(stderr, "cctext-render: %s in %.0f ms\n", in, now_ms() - t0);
    if (strlen(out) > 4 && !strcmp(out + strlen(out) - 4, ".svg")) {
        FILE *f = fopen(out, "wb");
        if (!f || fwrite(svg, 1, strlen(svg), f) != strlen(svg) || fclose(f) != 0) return 1;
        return 0;
    }
    return cr_svg_png_data(svg, strlen(svg), out, scale, dark ? 0x333333ffu : 0xffffffffu) == 0 ? 0 : 1;
}

/* --math IN OUT [display] [mml] [em=N] [SCALE]: development, no sandbox. */
static int dev_math(const char *pack, int argc, char **argv, int i)
{
    const char *in = argv[i + 1], *out = argv[i + 2];
    int display = 0, mml = 0;
    float scale = 1, em = 16;
    size_t n = 0;
    uint8_t *src;
    char err[1024], opts[256];
    const char *a[3];
    char *res, *nl;
    int to = 0;
    double t0;
    CrJs *e;
    for (int k = i + 3; k < argc; k++) {
        if (!strcmp(argv[k], "display")) display = 1;
        else if (!strcmp(argv[k], "mml")) mml = 1;
        else if (!strncmp(argv[k], "em=", 3)) em = (float)atof(argv[k] + 3);
        else if (atof(argv[k]) > 0) scale = (float)atof(argv[k]);
    }
    if (open_pack(pack) != 0 || fonts_init() < 1) return 2;
    src = slurp(in, &n);
    if (!src) {
        fprintf(stderr, "cctext-render: cannot read %s\n", in);
        return 2;
    }
    e = math_engine(err, sizeof err);
    if (!e) {
        fprintf(stderr, "cctext-render: %s\n", err);
        return 2;
    }
    snprintf(opts, sizeof opts, "{\"display\":%s,\"em\":%g,\"fg\":\"#000000\"}",
             display ? "true" : "false", (double)em);
    a[0] = mml ? "mml" : "tex";
    a[1] = (const char *)src;
    a[2] = opts;
    t0 = now_ms();
    if (strlen(out) > 4 && !strcmp(out + strlen(out) - 4, ".mml")) {
        /* TeX -> MathML text (testdata/math's MathML side). */
        const char *b[2] = {(const char *)src, opts};
        FILE *f;
        res = cr_js_call(e, "mjTex2mml", 2, b, 60000, err, sizeof err, &to);
        if (!res) {
            fprintf(stderr, "cctext-render: %s\n", err);
            return 1;
        }
        f = fopen(out, "wb");
        if (!f || fwrite(res, 1, strlen(res), f) != strlen(res) || fclose(f) != 0) return 1;
        return 0;
    }
    res = cr_js_call(e, "mjRender", 3, a, 60000, err, sizeof err, &to);
    if (!res) {
        fprintf(stderr, "cctext-render: %s\n", err);
        return 1;
    }
    fprintf(stderr, "cctext-render: %s in %.0f ms\n", in, now_ms() - t0);
    nl = strchr(res, '\n');
    if (!nl) {
        fprintf(stderr, "cctext-render: %s\n", res);
        return 1;
    }
    *nl = 0;
    fprintf(stderr, "cctext-render: %s\n", res);
    if (strlen(out) > 4 && !strcmp(out + strlen(out) - 4, ".svg")) {
        FILE *f = fopen(out, "wb");
        if (!f || fwrite(nl + 1, 1, strlen(nl + 1), f) != strlen(nl + 1) || fclose(f) != 0)
            return 1;
        return 0;
    }
    return cr_svg_png_data(nl + 1, strlen(nl + 1), out, scale, 0xffffffffu) == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    char pack_path[4096];
    const char *pack = NULL;
    int sandbox = 1;
#if defined(_WIN32)
    _setmode(0, _O_BINARY);
    _setmode(1, _O_BINARY);
#endif
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--build-pack") && i + 2 < argc)
            return cr_pack_build(argv[i + 1], argv[i + 2], cr_js_compile,
                                 i + 3 < argc ? argv[i + 3] : NULL);
#if defined(CR_BOOTSTRAP)
        fprintf(stderr, "cctext-render (bootstrap): only --build-pack\n");
        return 2;
#endif
        if (!strcmp(argv[i], "--version")) {
            printf("%s\n", CR_VERSION);
            return 0;
        }
        if (!strcmp(argv[i], "--no-sandbox")) sandbox = 0;
        else if (!strcmp(argv[i], "--pack") && i + 1 < argc) pack = argv[++i];
        else if (!strcmp(argv[i], "--recycle-mb") && i + 1 < argc) {
            long v = atol(argv[++i]);
            if (v > 0) g_policy.recycle_heap = (size_t)v << 20;
        } else if (!strcmp(argv[i], "--recycle-jobs") && i + 1 < argc) {
            long v = atol(argv[++i]);
            g_policy.recycle_jobs = v > 0 ? (unsigned)v : 0;
        } else if (!strcmp(argv[i], "--mermaid") && i + 2 < argc) {
            default_pack(pack_path, sizeof pack_path);
            return dev_mermaid(pack ? pack : pack_path, argc, argv, i);
        } else if (!strcmp(argv[i], "--math") && i + 2 < argc) {
            default_pack(pack_path, sizeof pack_path);
            return dev_math(pack ? pack : pack_path, argc, argv, i);
        } else if (!strcmp(argv[i], "--png") && i + 2 < argc) {
            /* development only: loads fonts, renders a file, no sandbox */
            const char *in = argv[i + 1], *out = argv[i + 2];
            float s = i + 3 < argc ? (float)atof(argv[i + 3]) : 1.0f;
            default_pack(pack_path, sizeof pack_path);
            if (open_pack(pack ? pack : pack_path) != 0) return 2;
            fonts_init();
            if (cr_svg_png(in, out, s > 0 ? s : 1.0f) != 0) {
                fprintf(stderr, "cctext-render: cannot render %s\n", in);
                return 1;
            }
            return 0;
        } else {
            fprintf(stderr, "usage: cctext-render [--pack PATH] [--recycle-mb N] [--recycle-jobs N] | "
                            "--build-pack MANIFEST OUT [HASH.h] | --png IN OUT [SCALE] | "
                            "--mermaid IN OUT [dark] [SCALE] | --version\n");
            return 2;
        }
    }
#if defined(CR_BOOTSTRAP)
    return 2;
#endif
    close_extra_fds();
    if (!pack) {
        default_pack(pack_path, sizeof pack_path);
        pack = pack_path;
    }
    if (open_pack(pack) != 0) return 2;
    if (fonts_init() < 1) {
        fprintf(stderr, "cctext-render: no fonts in %s\n", pack);
        return 2;
    }
    warm_up();
    if (sandbox) {
        if (cr_sandbox_lockdown(CR_MEM_LIMIT, CR_CPU_SECONDS) != 0) {
#if defined(__linux__) || defined(__APPLE__)
            fprintf(stderr, "cctext-render: lockdown failed; refusing to run\n");
            return 2;
#else
            g_disabled = 1; /* no sandbox here: say so and refuse to render */
            sandbox = 0;
#endif
        }
    }
#ifdef CR_SELFTEST
    if (getenv("CR_SELFTEST") && getenv("CR_SELFTEST")[0]) return selftest(getenv("CR_SELFTEST"));
#endif
    serve(sandbox);
    return 0;
}
