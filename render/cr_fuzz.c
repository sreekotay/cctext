/*
 * cctext-render fuzz driver (docs/images.md, "Renderer"): the SVG backend
 * (lunasvg + plutovg, the same code the helper runs) over mutated inputs,
 * in process, built with ASan + UBSan (scripts/render_build.cch, variant
 * "san" -> bin/cctext-render-fuzz). Any sanitizer report aborts the run.
 *
 *   cctext-render-fuzz PACK CORPUS_DIR [ITERS] [SEED]
 *   cctext-render-fuzz --mermaid PACK CORPUS_DIR [ITERS] [SEED]
 *
 * Every *.svg under CORPUS_DIR (and one level of subdirectories) is a
 * seed; each seed renders once unmutated, then ITERS mutants follow
 * (bit flips, truncation, duplicated chunks, extreme numbers, deep
 * nesting, self / mutual references, splices, attribute garbage, nested
 * percentage <svg>). Fixed hostile cases run first. The sandbox is not
 * involved (the policy has its own tests); this is about memory safety
 * and undefined behaviour in the parser and rasterizer.
 *
 * --mermaid: every *.mmd is a seed for the whole Mermaid path the helper
 * runs (QuickJS executing mermaid.min.js from the pack's bytecode, then
 * lunasvg on its SVG), under the same sanitizers (QuickJS's C included):
 * line deletions / duplications / swaps, token and arrow garbage, splices
 * between diagram types, long labels and ids, deep subgraph nesting,
 * directives (%%{init}%% with hostile config), click / href / callback
 * lines, HTML and script in labels, classDef CSS garbage, control bytes
 * and truncations. Each mutant has the editor's limits: 150 nodes and a
 * time budget (an interrupted script is an outcome, not a finding). The
 * engine restarts after every failure, as in the helper.
 *
 *   cctext-render-fuzz --math PACK testdata/math [ITERS] [SEED]
 *
 * --math: the TeX formulas of formulas.json and the MathML files of mml/
 * are seeds for MathJax in QuickJS (the math bundle's bytecode and its
 * source modules from the pack), then lunasvg on the SVG: byte flips,
 * truncation, TeX / MathML token garbage (\def recursion, \newcommand,
 * huge \hspace / \rule / \kern, \unicode, refused extensions, raw HTML,
 * entities, CDATA), deep nesting (\frac, groups, \sqrt, subscripts,
 * \left, <mrow>, <msqrt>), duplicated chunks, macro doubling chains,
 * long \text and many terms, control bytes and invalid UTF-8, extreme
 * numbers and splices; inline or display, some with a max width. The
 * editor's limits apply (4000 nodes, maxMacros, the time budget).
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "cr_js.h"
#include "cr_pack.h"
#include "cr_svg.h"

typedef struct {
    char *b;
    size_t n, cap;
} Buf;

static void buf_add(Buf *o, const char *s, size_t n)
{
    if (o->n + n + 1 > o->cap) {
        size_t c = (o->n + n + 1) * 2;
        char *nb = realloc(o->b, c);
        if (!nb) abort();
        o->b = nb;
        o->cap = c;
    }
    memcpy(o->b + o->n, s, n);
    o->n += n;
    o->b[o->n] = 0;
}

static void buf_str(Buf *o, const char *s) { buf_add(o, s, strlen(s)); }

static uint64_t g_rng = 88172645463325252ull;
static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 11);
}
static size_t rndn(size_t n) { return n ? rnd() % n : 0; }

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

typedef struct {
    char *d;
    size_t n;
    char name[256];
} Seed;

static Seed g_seed[512];
static int g_nseed;

static const char *g_ext = ".svg";

static void load_dir(const char *dir, int depth)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d)) && g_nseed < 512) {
        char p[2048];
        struct stat st;
        size_t l = strlen(e->d_name);
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 1) load_dir(p, depth + 1);
            continue;
        }
        if (l < 5 || strcmp(e->d_name + l - 4, g_ext) != 0 || st.st_size > (1 << 20)) continue;
        {
            FILE *f = fopen(p, "rb");
            Seed *s = &g_seed[g_nseed];
            if (!f) continue;
            s->d = malloc((size_t)st.st_size + 1);
            s->n = fread(s->d, 1, (size_t)st.st_size, f);
            s->d[s->n] = 0;
            fclose(f);
            snprintf(s->name, sizeof s->name, "%s", e->d_name);
            g_nseed++;
        }
    }
    closedir(d);
}

static double g_worst_ms;
static char g_worst[64];
static long g_ok, g_rej;

static void render_one(const char *what, const char *s, size_t n)
{
    float w = 0, h = 0;
    double t0 = now_ms(), dt;
    cr_svg *doc = cr_svg_parse(s, n, &w, &h);
    if (doc) {
        /* The helper's own checks, then a small box (fast under ASan). */
        if (w > 0 && h > 0 && w < 1e7f && h < 1e7f) {
            /* double: 192 / a denormal side overflows a float to inf */
            double sc = 192.0 / (w > h ? w : h);
            double fw = ceil((double)w * sc), fh = ceil((double)h * sc);
            uint32_t pw = fw < 1 ? 1 : fw > 192 ? 192 : (uint32_t)fw;
            uint32_t ph = fh < 1 ? 1 : fh > 192 ? 192 : (uint32_t)fh;
            uint8_t *px;
            px = malloc((size_t)pw * ph * 4);
            if (px) {
                cr_svg_render(doc, (float)pw / w, (float)ph / h, pw, ph, 0x00000000u, px);
                free(px);
            }
        }
        cr_svg_free(doc);
        g_ok++;
    } else {
        g_rej++;
    }
    dt = now_ms() - t0;
    if (dt > g_worst_ms) {
        g_worst_ms = dt;
        snprintf(g_worst, sizeof g_worst, "%s", what);
    }
}

static const char *const NUMS[] = {"0",        "-1",         "1e38", "-1e38", "1e-38", "NaN", "inf",
                                   "99999999", "-99999999",  "0.0000001",   "4294967296", "1e400",
                                   ""};
static const char *const TAGS[] = {"g", "svg", "text", "tspan", "switch", "a", "mask", "symbol"};
static const char *const GARBAGE[] = {"", "(", "url(#", "calc(", "%", "e", "1e", "#", ";;;", "\\"};

static const char *snippet(Buf *o)
{
    switch (rndn(21)) {
    case 13: /* filters: huge blur on a huge shape, huge offsets */
        return "<filter id=\"fb\" filterUnits=\"userSpaceOnUse\" x=\"-1e9\" y=\"-1e9\" "
               "width=\"2e9\" height=\"2e9\"><feGaussianBlur stdDeviation=\"1e30 3e38\"/>"
               "<feOffset dx=\"1e38\" dy=\"-1e38\"/></filter><rect filter=\"url(#fb)\" "
               "width=\"1e6\" height=\"1e6\"/><circle r=\"5\" filter=\"url(#fb)\"/>";
    case 14: { /* many primitives, named results read back and forth */
        int i;
        char t[160];
        buf_str(o, "<filter id=\"fm\">");
        for (i = 0; i < 400; i++) {
            snprintf(t, sizeof t,
                     "<feGaussianBlur stdDeviation=\"%d\" result=\"r%d\"/><feComposite in=\"r%d\" "
                     "in2=\"SourceAlpha\" operator=\"arithmetic\" k1=\"1e38\" k4=\"-1\"/>",
                     i % 7, i, i / 2);
            buf_str(o, t);
        }
        buf_str(o, "</filter><rect filter=\"url(#fm)\" width=\"50\" height=\"50\"/>");
        return NULL;
    }
    case 15: /* recursive filter references: href cycles, filtered content inside */
        return "<filter id=\"f1\" href=\"#f2\"/><filter id=\"f2\" xlink:href=\"#f1\" "
               "filter=\"url(#f1)\"><feFlood filter=\"url(#f2)\"/></filter><filter id=\"f3\" "
               "href=\"#f3\"/><g filter=\"url(#f1)\"><g filter=\"url(#f2)\"><rect "
               "filter=\"url(#f3)\" width=\"9\" height=\"9\"/></g></g>";
    case 16: /* zero / negative / inverted regions and subregions */
        return "<filter id=\"fz\" x=\"0\" y=\"0\" width=\"0\" height=\"-5\"><feFlood/></filter>"
               "<filter id=\"fn\" primitiveUnits=\"objectBoundingBox\"><feFlood x=\"-1e38\" "
               "width=\"-1\"/><feOffset x=\"1e38\" width=\"1e38\" dx=\"NaN\"/><feMerge><feMergeNode/>"
               "<feMergeNode in=\"nope\"/></feMerge></filter><rect filter=\"url(#fz)\" width=\"9\" "
               "height=\"9\"/><line x2=\"9\" filter=\"url(#fn)\"/><rect filter=\"url(#fn)\" "
               "width=\"9\" height=\"9\"/>";
    case 17: { /* nested filtered groups */
        int i;
        for (i = 0; i < 20; i++) buf_str(o, "<g filter=\"url(#fd)\" opacity=\"0.9\">");
        buf_str(o, "<rect width=\"100\" height=\"100\"/>");
        for (i = 0; i < 20; i++) buf_str(o, "</g>");
        buf_str(o, "<filter id=\"fd\"><feDropShadow stdDeviation=\"3\" dx=\"2\" dy=\"2\"/></filter>");
        return NULL;
    }
    case 18: /* CSS filter functions with extreme values and bad urls */
        return "<rect width=\"50\" height=\"50\" filter=\"blur(1e30px) drop-shadow(1e30 -1e30 1e30 "
               "red) url(#nope) url(#fb) hue-rotate(1e30deg) contrast(1e38) saturate(-1) "
               "opacity(NaN)\"/><rect width=\"9\" height=\"9\" style=\"filter: drop-shadow(\"/>";
    case 19: { /* big lookup tables and matrices */
        int i;
        buf_str(o, "<filter id=\"ft\"><feComponentTransfer><feFuncR type=\"table\" tableValues=\"");
        for (i = 0; i < 20000; i++) buf_str(o, i & 1 ? "1e38 " : "-1e38 ");
        buf_str(o, "\"/><feFuncA type=\"gamma\" exponent=\"-1e38\" amplitude=\"1e38\"/>"
                   "</feComponentTransfer><feColorMatrix values=\"1e38 1e38 1e38 1e38 1e38 1e38 1e38 1e38 "
                   "1e38 1e38 1e38 1e38 1e38 1e38 1e38 1e38 1e38 1e38 1e38 1e38\"/><feColorMatrix "
                   "type=\"hueRotate\" values=\"1e38\"/></filter><rect filter=\"url(#ft)\" "
                   "width=\"20\" height=\"20\"/>");
        return NULL;
    }
    case 20: /* a filter on the root, on a transformed and a zero-sized element */
        return "<filter id=\"fr\"><feGaussianBlur stdDeviation=\"2\"/><feBlend mode=\"hue\" "
               "in2=\"SourceGraphic\"/></filter><g transform=\"rotate(45) scale(1e-30 1e30)\" "
               "filter=\"url(#fr)\"><rect width=\"10\" height=\"10\"/></g><rect width=\"0\" "
               "height=\"0\" filter=\"url(#fr)\"/>";
    case 0: return "<g id=\"a\"><use href=\"#a\"/></g>";
    case 1: return "<use id=\"b\" href=\"#c\"/><use id=\"c\" href=\"#b\"/>";
    case 2:
        return "<pattern id=\"p\" width=\"10\" height=\"10\"><rect fill=\"url(#p)\" width=\"5\" "
               "height=\"5\"/></pattern><rect fill=\"url(#p)\" width=\"50\" height=\"50\"/>";
    case 3:
        return "<mask id=\"m\"><rect mask=\"url(#m)\" width=\"10\" height=\"10\"/></mask><rect "
               "mask=\"url(#m)\" width=\"9\" height=\"9\"/>";
    case 4:
        return "<clipPath id=\"cp\" clip-path=\"url(#cp)\"><rect width=\"10\" "
               "height=\"10\"/></clipPath><rect clip-path=\"url(#cp)\" width=\"9\" height=\"9\"/>";
    case 5:
        return "<marker id=\"mk\"><path d=\"M0 0L9 9\" marker-start=\"url(#mk)\"/></marker><path "
               "d=\"M0 0L9 9L20 3\" marker-start=\"url(#mk)\" marker-mid=\"url(#mk)\"/>";
    case 6: {
        int i;
        buf_str(o, "<path stroke=\"black\" stroke-dasharray=\"0.5\" d=\"M0 0");
        for (i = 0; i < 2000; i++) buf_str(o, " L1 1 L2 0");
        buf_str(o, "\"/>");
        return NULL;
    }
    case 7: return "<rect width=\"1e9\" height=\"1e9\" stroke-width=\"1e9\" stroke=\"red\"/>";
    case 8: {
        int i;
        buf_str(o, "<text font-size=\"1e6\">");
        for (i = 0; i < 500; i++) buf_str(o, "W");
        buf_str(o, "</text>");
        return NULL;
    }
    case 9: {
        int i;
        buf_str(o, "<text x=\"");
        for (i = 0; i < 5000; i++) buf_str(o, "1 ");
        buf_str(o, "\">abc</text>");
        return NULL;
    }
    case 10: {
        int i;
        buf_str(o, "<linearGradient id=\"lg\">");
        for (i = 0; i < 2000; i++) buf_str(o, "<stop offset=\"0.5\"/>");
        buf_str(o, "</linearGradient><rect fill=\"url(#lg)\" width=\"9\" height=\"9\"/>");
        return NULL;
    }
    case 11:
        return "<image width=\"10\" height=\"10\" href=\"data:image/png;base64,iVBORw0KGgoAAAANSU"
               "hEUgAAAAIAAAACCAYAAABytg0kAAAAE0lEQVR4nGP4z8AAQmDqP5D6\"/>";
    default:
        return "<image width=\"10\" height=\"10\" href=\"file:///etc/passwd\"/><image width=\"10\" "
               "height=\"10\" href=\"http://127.0.0.1:9/x.png\"/>";
    }
}

/* One mutant of seed s into o. */
static void mutate(const Seed *s, Buf *o)
{
    size_t n = s->n, i, j;
    const char *b = s->d;
    o->n = 0;
    switch (rndn(9)) {
    case 0: /* byte flips */
        buf_add(o, b, n);
        for (i = rndn(20) + 1; i-- && o->n;) o->b[rndn(o->n)] = (char)rnd();
        break;
    case 1: /* truncate */
        buf_add(o, b, rndn(n + 1));
        break;
    case 2: { /* duplicate a chunk */
        size_t a = rndn(n + 1), e = a + rndn(400) + 1, k = rndn(50) + 1;
        if (e > n) e = n;
        buf_add(o, b, e);
        while (k--) buf_add(o, b + a, e - a);
        buf_add(o, b + e, n - e);
        break;
    }
    case 3: /* extreme numbers */
        for (i = 0; i < n;) {
            if ((b[i] >= '0' && b[i] <= '9') || (b[i] == '-' && i + 1 < n && b[i + 1] >= '0' &&
                                                 b[i + 1] <= '9')) {
                j = i + 1;
                while (j < n && ((b[j] >= '0' && b[j] <= '9') || b[j] == '.')) j++;
                if (rndn(10) < 3) buf_str(o, NUMS[rndn(sizeof NUMS / sizeof NUMS[0])]);
                else buf_add(o, b + i, j - i);
                i = j;
            } else {
                buf_add(o, b + i, 1);
                i++;
            }
        }
        break;
    case 4: { /* deep nesting after the root's start tag */
        const char *r = strstr(b, "<svg");
        const char *gt = r ? strchr(r, '>') : NULL;
        size_t at = gt ? (size_t)(gt + 1 - b) : 0, d, k;
        static const size_t depths[] = {100, 1000, 5000};
        const char *t = TAGS[rndn(sizeof TAGS / sizeof TAGS[0])];
        char tag[32];
        d = depths[rndn(3)];
        buf_add(o, b, at);
        for (k = 0; k < d; k++) {
            snprintf(tag, sizeof tag, "<%s>", t);
            buf_str(o, tag);
        }
        buf_str(o, "<rect width=\"10\" height=\"10\"/>");
        for (k = 0; k < d; k++) {
            snprintf(tag, sizeof tag, "</%s>", t);
            buf_str(o, tag);
        }
        buf_add(o, b + at, n - at);
        break;
    }
    case 5: { /* self / mutual references and other hostile snippets */
        const char *r = strstr(b, "<svg");
        const char *gt = r ? strchr(r, '>') : NULL;
        size_t at = gt ? (size_t)(gt + 1 - b) : 0;
        const char *sn;
        buf_add(o, b, at);
        sn = snippet(o);
        if (sn) buf_str(o, sn);
        buf_add(o, b + at, n - at);
        break;
    }
    case 6: { /* splice from another seed */
        const Seed *t = &g_seed[rndn((size_t)g_nseed)];
        size_t a = rndn(n + 1), f = rndn(t->n + 1), l = rndn(2000) + 1;
        if (f + l > t->n) l = t->n - f;
        buf_add(o, b, a);
        buf_add(o, t->d + f, l);
        buf_add(o, b + a, n - a);
        break;
    }
    case 7: /* attribute garbage */
        for (i = 0; i < n; i++) {
            if (b[i] == '=' && i + 1 < n && b[i + 1] == '"' && rndn(100) < 15) {
                j = i + 2;
                while (j < n && b[j] != '"') j++;
                buf_str(o, "=\"");
                if (rndn(4) == 0) {
                    size_t k;
                    for (k = 0; k < 3000; k++) buf_str(o, "a");
                } else if (rndn(4) == 0 && j > i + 2) {
                    size_t k;
                    for (k = 0; k < 50; k++) buf_add(o, b + i + 2, j - i - 2);
                } else {
                    buf_str(o, GARBAGE[rndn(sizeof GARBAGE / sizeof GARBAGE[0])]);
                }
                i = j - 1;
                continue;
            }
            buf_add(o, b + i, 1);
        }
        break;
    default: { /* nested percentage <svg> (was 2^depth before patch 0003) */
        size_t k, d = 20 + rndn(30);
        buf_str(o, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\">");
        for (k = 0; k < d; k++) buf_str(o, "<svg width=\"90%\" height=\"90%\">");
        buf_str(o, "<rect width=\"50%\" height=\"50%\" fill=\"red\"/>");
        for (k = 0; k < d; k++) buf_str(o, "</svg>");
        buf_str(o, "</svg>");
        break;
    }
    }
}

/* As in the helper: faces over 1 MiB are fallbacks loaded on first use. */
static const void *lazy_font(void *closure, size_t *n)
{
    CrAsset *a = (CrAsset *)closure;
    const uint8_t *d = cr_asset_data(a);
    *n = d ? a->raw_len : 0;
    return d;
}

static int fonts_init(CrPack *p)
{
    int nf = 0;
    for (int i = 0; i < p->n; i++) {
        CrAsset *a = &p->e[i];
        char fam[256];
        int bold = 0, italic = 0;
        const char *s, *c1;
        const uint8_t *d;
        if (strncmp(a->name, "font:", 5) != 0) continue;
        s = a->name + 5;
        c1 = strchr(s, ':');
        if (!c1 || (size_t)(c1 - s) >= sizeof fam) continue;
        memcpy(fam, s, (size_t)(c1 - s));
        fam[c1 - s] = 0;
        if (sscanf(c1 + 1, "%d:%d", &bold, &italic) != 2) continue;
        if (a->raw_len > (1u << 20)) {
            if (cr_svg_add_lazy_fallback(lazy_font, a) == 0) nf++;
            continue;
        }
        d = cr_asset_data(a);
        if (d && cr_svg_add_font(fam, bold, italic, d, a->raw_len) == 0) nf++;
    }
    return nf;
}

/* ---- Mermaid ------------------------------------------------------------ */

static CrJs *g_mm;
static long g_mm_ok, g_mm_err, g_mm_timeout, g_mm_large;

static void mm_one(const char *what, const char *src, size_t n, uint32_t budget_ms, int dark)
{
    static const char opts_l[] = "{\"nodeMax\":150,\"theme\":\"default\"}";
    static const char opts_d[] =
        "{\"nodeMax\":150,\"theme\":\"dark\",\"themeVariables\":{\"primaryColor\":\"#223344\"}}";
    char err[1024], *svg, *z;
    const char *a[3];
    int to = 0;
    double t0 = now_ms(), dt;
    z = malloc(n + 1);
    if (!z) abort();
    memcpy(z, src, n);
    z[n] = 0; /* a NUL inside ends the source (as a JS string from the helper) */
    a[0] = "cctext-mermaid";
    a[1] = z;
    a[2] = dark ? opts_d : opts_l;
    svg = cr_js_call(g_mm, "mmRender", 3, a, budget_ms, err, sizeof err, &to);
    if (svg && !strncmp(svg, "CCTEXT_TOO_LARGE", 16)) g_mm_large++;
    else if (svg && !strncmp(svg, "CCTEXT_PARSE ", 13)) g_mm_err++; /* engine kept */
    else if (svg) {
        g_mm_ok++;
        render_one(what, svg, strlen(svg));
    } else if (to) g_mm_timeout++;
    else g_mm_err++;
    free(svg);
    free(z);
    dt = now_ms() - t0;
    if (dt > g_worst_ms) {
        g_worst_ms = dt;
        snprintf(g_worst, sizeof g_worst, "%s", what);
    }
}

static const char *const MM_TOK[] = {
    "-->", "---", "-.->", "==>", "--x", "--o", "<-->", "->>", "-->>", "-x", "-)", "||--o{", "}|..|{",
    "<|--", "*--", "o--", "..>", "[*]", "((", "))", "{{", "}}", "[/", "/]", "[(", ")]", ">", "|",
    ":", ";", "\"", "'", "`", "%%", "#", "&", "<br>", "<b>", "<script>alert(1)</script>",
    "<img src=x onerror=alert(1)>", "javascript:alert(1)", "\\u0000", "\xef\xbf\xbf", "\xf0\x9f\x98\x80",
    "\xe2\x80\xae", "subgraph", "end", "classDef", "class", "style", "linkStyle", "click", "call",
    "href", "note", "loop", "alt", "else", "opt", "par", "and", "rect", "activate", "deactivate",
    "section", "dateFormat", "axisFormat", "excludes", "after", "crit", "done", "active", "title",
    "direction", "TB", "LR", "RL", "BT", "state", "fork", "join", "choice", "<<interface>>",
    "1e308", "-1", "NaN", "9999999999", "0x7fffffff", "2020-13-45", "::", ":::", "~T~", "{", "}"};

static const char *const MM_LINES[] = {
    "%%{init: {\"securityLevel\": \"loose\", \"htmlLabels\": true, \"startOnLoad\": true}}%%",
    "%%{init: {\"theme\": \"forest\", \"themeCSS\": \"* { font-size: 1e9px } svg { width: 1e9px }\"}}%%",
    "%%{init: {\"flowchart\": {\"htmlLabels\": true, \"curve\": \"__proto__\"}, \"fontSize\": 1e9}}%%",
    "%%{init: {\"maxTextSize\": 1e12, \"maxEdges\": 1e12, \"deterministicIDSeed\": \"<svg\"}}%%",
    "%%{wrap}%%",
    "---\nconfig:\n  theme: dark\n  look: handDrawn\n  layout: elk\n---",
    "---\nconfig:\n  layout: elk\n---",
    "  click A callback \"tip\"", "  click A href \"javascript:alert(1)\" _blank",
    "  click A call alert(1)", "  A[\"<a href='javascript:alert(1)'>x</a>\"]",
    "  classDef x fill:url(#a),stroke:expression(alert(1)),font-size:1e9px;",
    "  style A fill:#f9f,stroke:#333,stroke-width:1e9px",
    "  linkStyle 999999 stroke:red", "  A@{ shape: cyl, label: \"x\" }",
    "  A@{ icon: \"fa:user\", img: \"https://example.com/x.png\", w: 1e9 }",
    "  accTitle: <script>", "  accDescr: x", "  note right of A: <b>x</b>",
    "  A -->|\"<img src=x>\"| B"};

static void mm_mutate(const Seed *s, Buf *o)
{
    const char *b = s->d;
    size_t n = s->n, i, k;
    o->n = 0;
    switch (rndn(12)) {
    case 0: /* byte flips */
        buf_add(o, b, n);
        for (i = rndn(8) + 1; i-- && o->n;) o->b[rndn(o->n)] = (char)rnd();
        break;
    case 1: /* truncate */
        buf_add(o, b, rndn(n + 1));
        break;
    case 2: { /* delete / duplicate / swap lines */
        const char *ls[256];
        size_t ll[256], nl = 0, p = 0;
        while (p < n && nl < 256) {
            size_t e = p;
            while (e < n && b[e] != '\n') e++;
            ls[nl] = b + p;
            ll[nl++] = e - p;
            p = e + 1;
        }
        for (i = 0; i < nl; i++) {
            size_t r = rndn(10), j = i;
            if (r == 0) continue;                /* delete */
            if (r == 1) j = rndn(nl);             /* another line instead */
            buf_add(o, ls[j], ll[j]);
            buf_str(o, "\n");
            if (r == 2)
                for (k = rndn(20) + 1; k--;) { buf_add(o, ls[j], ll[j]); buf_str(o, "\n"); }
        }
        break;
    }
    case 3: /* token garbage at random places */
        buf_add(o, b, n);
        for (i = rndn(6) + 1; i--;) {
            const char *t = MM_TOK[rndn(sizeof MM_TOK / sizeof MM_TOK[0])];
            size_t at = rndn(o->n + 1), tl = strlen(t);
            buf_add(o, t, tl);                    /* grow, then move the tail */
            memmove(o->b + at + tl, o->b + at, o->n - tl - at);
            memcpy(o->b + at, t, tl);
        }
        break;
    case 4: { /* hostile lines after the header */
        const char *nl = memchr(b, '\n', n);
        size_t at = nl ? (size_t)(nl + 1 - b) : n;
        if (rndn(3) == 0) {
            buf_str(o, MM_LINES[rndn(4)]);
            buf_str(o, "\n");
            buf_add(o, b, n);
            break;
        }
        buf_add(o, b, at);
        for (i = rndn(3) + 1; i--;) {
            buf_str(o, MM_LINES[rndn(sizeof MM_LINES / sizeof MM_LINES[0])]);
            buf_str(o, "\n");
        }
        buf_add(o, b + at, n - at);
        break;
    }
    case 5: { /* splice another type's lines in */
        const Seed *t = &g_seed[rndn((size_t)g_nseed)];
        size_t a = rndn(n + 1), f = rndn(t->n + 1), l = rndn(300) + 1;
        if (f + l > t->n) l = t->n - f;
        buf_add(o, b, a);
        buf_add(o, t->d + f, l);
        buf_add(o, b + a, n - a);
        break;
    }
    case 6: { /* long labels / ids */
        size_t at = rndn(n + 1), L = (size_t)1 << (8 + rndn(9));
        buf_add(o, b, at);
        for (k = 0; k < L; k++) buf_add(o, (rndn(20) ? "W" : " "), 1);
        buf_add(o, b + at, n - at);
        break;
    }
    case 7: { /* deep subgraph / state / namespace nesting */
        static const size_t depths[] = {20, 100, 400};
        size_t d = depths[rndn(3)];
        int kind = (int)rndn(3);
        buf_str(o, kind == 0 ? "flowchart TD\n" : kind == 1 ? "stateDiagram-v2\n" : "classDiagram\n");
        for (k = 0; k < d; k++) {
            char l[64];
            if (kind == 0) snprintf(l, sizeof l, "subgraph s%zu\n", k);
            else if (kind == 1) snprintf(l, sizeof l, "state S%zu {\n", k);
            else snprintf(l, sizeof l, "namespace N%zu {\n", k);
            buf_str(o, l);
        }
        buf_str(o, kind == 0 ? "a --> b\n" : kind == 1 ? "[*] --> x\n" : "class A\n");
        for (k = 0; k < d; k++) buf_str(o, kind == 0 ? "end\n" : "}\n");
        break;
    }
    case 8: { /* many nodes / edges around the cap */
        size_t m = 100 + rndn(200);
        buf_str(o, "flowchart LR\n");
        for (k = 0; k < m; k++) {
            char l[64];
            snprintf(l, sizeof l, "n%zu --> n%zu\n", rndn(m / 2 + 1), rndn(m));
            buf_str(o, l);
        }
        break;
    }
    case 9: /* control bytes and invalid UTF-8 */
        for (i = 0; i < n; i++) {
            buf_add(o, b + i, 1);
            if (rndn(40) == 0) {
                static const char junk[] = "\x01\x7f\xc0\xff\xed\xa0\x80\x00\r\t\x1b";
                buf_add(o, junk + rndn(sizeof junk - 1), 1);
            }
        }
        break;
    case 10: /* extreme numbers (gantt dates, pie values, sizes) */
        for (i = 0; i < n; i++) {
            if (b[i] >= '0' && b[i] <= '9' && rndn(4) == 0) {
                while (i + 1 < n && b[i + 1] >= '0' && b[i + 1] <= '9') i++;
                buf_str(o, NUMS[rndn(sizeof NUMS / sizeof NUMS[0])]);
            } else {
                buf_add(o, b + i, 1);
            }
        }
        break;
    default: /* the seed unchanged, in the other theme */
        buf_add(o, b, n);
        break;
    }
}

static int mm_main(int argc, char **argv, CrPack *pack)
{
    long iters = argc > 3 ? atol(argv[3]) : 200, i;
    uint32_t budget = getenv("CR_FUZZ_MM_BUDGET_MS") ? (uint32_t)atol(getenv("CR_FUZZ_MM_BUDGET_MS"))
                                                     : 20000u;
    CrAsset *a = cr_pack_find(pack, "js:mermaid");
    const uint8_t *bc = a ? cr_asset_data(a) : NULL;
    CrJsPolicy pol = {(size_t)1 << 30, (size_t)256 << 20, 0, (size_t)16 << 20};
    Buf o = {0};
    double t0 = now_ms();
    if (!bc) {
        fprintf(stderr, "cctext-render-fuzz: no js:mermaid in the pack\n");
        return 2;
    }
    g_mm = cr_js_new("mermaid", bc, a->raw_len, &pol);
    g_ext = ".mmd";
    load_dir(argv[2], 0);
    if (!g_nseed) {
        fprintf(stderr, "cctext-render-fuzz: no *.mmd under %s\n", argv[2]);
        return 2;
    }
    for (i = 0; i < g_nseed && !getenv("CR_FUZZ_DUMP_ONLY"); i++)
        mm_one(g_seed[i].name, g_seed[i].d, g_seed[i].n, budget > 60000 ? budget : 60000,
               (int)(i & 1)); /* seeds must render: a generous budget under ASan */
    if (g_mm_ok < g_nseed && !getenv("CR_FUZZ_DUMP_ONLY")) {
        fprintf(stderr, "cctext-render-fuzz: only %ld of %d Mermaid seeds rendered\n", g_mm_ok,
                g_nseed);
        return 1;
    }
    for (i = 0; i < iters; i++) {
        const Seed *s = &g_seed[rndn((size_t)g_nseed)];
        char what[64];
        int dark = (int)rndn(2);
        mm_mutate(s, &o);
        snprintf(what, sizeof what, "mermaid #%ld of %s", i, s->name);
        if (getenv("CR_FUZZ_DUMP") && (atol(getenv("CR_FUZZ_DUMP")) == i ||
                                       !strcmp(getenv("CR_FUZZ_DUMP"), "all"))) {
            /* CR_FUZZ_DUMP=N (or all) writes the mutant to cr-fuzz-N.mmd */
            char p[64];
            FILE *df;
            snprintf(p, sizeof p, "cr-fuzz-%ld.mmd", i);
            df = fopen(p, "wb");
            if (df) {
                fwrite(o.b ? o.b : "", 1, o.n, df);
                fclose(df);
            }
        }
        /* CR_FUZZ_DUMP_ONLY: write the dumped mutant, render nothing */
        if (!getenv("CR_FUZZ_DUMP_ONLY")) mm_one(what, o.b ? o.b : "", o.n, budget, dark);
    }
    {
        CrJsStats st;
        cr_js_stats(g_mm, &st);
        printf("cctext-render-fuzz --mermaid: %d seeds, %ld mutants: %ld rendered, %ld errors, %ld "
               "over the node cap, %ld timeouts, %u engine starts, %.1f s; slowest %.0f ms (%s)\n",
               g_nseed, iters, g_mm_ok - g_nseed, g_mm_err, g_mm_large, g_mm_timeout, st.starts,
               (now_ms() - t0) / 1000.0, g_worst_ms, g_worst);
    }
    cr_js_stop(g_mm);
    free(g_mm);
    free(o.b);
    return 0;
}

/* ---- math (--math): MathJax in QuickJS, then lunasvg ----------------------- */

static CrJs *g_mj;
static CrPack *g_fpack;
static long g_mj_ok, g_mj_err, g_mj_timeout, g_mj_large;

/* The helper's source modules (cctext-render.c checks their SHA-256; the
 * fuzz build has no hash header: it reads its own pack). */
static int fz_src_get(void *ctx, const char *key, const char **src, size_t *n, char *err,
                      size_t cap)
{
    char name[300];
    CrAsset *a;
    (void)ctx;
    (void)err;
    (void)cap;
    snprintf(name, sizeof name, "src:%s", key);
    a = cr_pack_find(g_fpack, name);
    if (!a || !(*src = (const char *)cr_asset_data(a))) return 1;
    *n = a->raw_len;
    return 0;
}

static void fz_src_put(void *ctx, const char *key)
{
    char name[300];
    (void)ctx;
    snprintf(name, sizeof name, "src:%s", key);
    cr_asset_drop(cr_pack_find(g_fpack, name));
}

static void mj_one(const char *what, const char *src, size_t n, uint32_t budget_ms, int mml,
                   int display)
{
    char err[1024], opts[160], *res, *z;
    const char *a[3];
    int to = 0;
    double t0 = now_ms(), dt;
    z = malloc(n + 1);
    if (!z) abort();
    memcpy(z, src, n);
    z[n] = 0;
    snprintf(opts, sizeof opts,
             "{\"display\":%s,\"em\":16,\"fg\":\"#000000\",\"maxWidth\":%d,\"nodeMax\":4000}",
             display ? "true" : "false", rndn(4) == 0 ? 300 : 0);
    a[0] = mml ? "mml" : "tex";
    a[1] = z;
    a[2] = opts;
    res = cr_js_call(g_mj, "mjRender", 3, a, budget_ms, err, sizeof err, &to);
    if (res && !strncmp(res, "CCTEXT_TOO_LARGE", 16)) g_mj_large++;
    else if (res && !strncmp(res, "CCTEXT_ERR ", 11)) g_mj_err++; /* engine kept */
    else if (res) {
        const char *nl = strchr(res, '\n');
        g_mj_ok++;
        if (nl) render_one(what, nl + 1, strlen(nl + 1));
    } else if (to) g_mj_timeout++;
    else g_mj_err++;
    free(res);
    free(z);
    dt = now_ms() - t0;
    if (dt > g_worst_ms) {
        g_worst_ms = dt;
        snprintf(g_worst, sizeof g_worst, "%s", what);
    }
}

static const char *const MJ_TOK[] = {
    "\\frac", "\\sqrt", "{", "}", "^", "_", "\\left(", "\\right)", "\\left.", "\\right|",
    "\\begin{array}{c}", "\\end{array}", "\\begin{pmatrix}", "\\end{pmatrix}", "&", "\\\\",
    "\\def\\a{\\a\\a}\\a", "\\def\\a#1{#1#1}", "\\newcommand{\\x}[1]{#1#1}", "\\let\\frac\\sqrt",
    "\\hspace{1e6em}", "\\hspace{-1e6em}", "\\rule{1e5em}{1e5em}", "\\rule{-1em}{NaNem}",
    "\\color{#zzz}", "\\color{red}", "\\text{", "\\mathbb", "\\require{", "\\require{html}",
    "\\href{javascript:alert(1)}{", "\\unicode{0x10FFFF}", "\\unicode{0}", "\\char\"FFFFFF",
    "\\mathchoice{a}{b}{c}{d}", "\\overbrace", "\\underbrace", "\\xrightarrow[", "\\operatorname",
    "\\big", "\\Huge", "\\tiny", "\\kern-1e9em", "\\raise1e9em", "\\scriptstyle", "%", "#", "$",
    "~", "\\", "\xef\xbf\xbf", "\xf0\x9f\x98\x80", "\xe2\x80\xae", "\xe9\x9d\xa2", "\\label{a}",
    "\\tag{1}", "\\eqref{x}", "\\begin{align}", "\\end{align}", "\\cancel{", "\\boxed{",
    "\\phantom{", "\\mathrm{", "\\begingroup", "\\endgroup", "\\global\\def\\q{q}", "\\mmlToken{mi}{x}",
    "\\class{x}{", "\\style{color:red}{", "\\cssId{x}{", "\\mathtip{a}{b}", "\\verb|x|",
    "\\ce{H2O}", "\\bbox[red,5px]{", "\\enclose{circle}{", "\\bra{a}", "\\qty(", "\\SI{3}{m}"};

static const char *const MJ_MML[] = {
    "<mrow>", "</mrow>", "<mi>x</mi>", "<mfrac>", "</mfrac>", "<msqrt>", "</msqrt>",
    "<mspace width=\"1e9em\"/>", "<mpadded width=\"1e9em\" height=\"-1e9em\">", "</mpadded>",
    "<mstyle mathsize=\"1e6em\">", "</mstyle>", "<mo stretchy=\"true\" minsize=\"1e9em\">(</mo>",
    "<mtable><mtr><mtd>", "</mtd></mtr></mtable>", "<mmultiscripts>", "<mprescripts/>",
    "<semantics><annotation-xml encoding=\"text/html\"><script>1</script></annotation-xml>",
    "<maction actiontype=\"toggle\">", "<merror>", "&alpha;", "&NotExisting;", "&#x110000;",
    "<![CDATA[", "]]>", "<!--", "-->", "<math", ">", "</math>", "href=\"javascript:1\"",
    "<menclose notation=\"box circle\">", "<mglyph src=\"file:///etc/passwd\"/>"};

static void mj_mutate(const Seed *s, int mml, Buf *o)
{
    const char *b = s->d;
    size_t n = s->n, i, k;
    const char *const *tok = mml ? MJ_MML : MJ_TOK;
    size_t ntok = mml ? sizeof MJ_MML / sizeof MJ_MML[0] : sizeof MJ_TOK / sizeof MJ_TOK[0];
    o->n = 0;
    switch (rndn(10)) {
    case 0: /* byte flips */
        buf_add(o, b, n);
        for (i = rndn(6) + 1; i-- && o->n;) o->b[rndn(o->n)] = (char)rnd();
        break;
    case 1: /* truncate */
        buf_add(o, b, rndn(n + 1));
        break;
    case 2: /* tokens at random places */
        buf_add(o, b, n);
        for (i = rndn(6) + 1; i--;) {
            const char *t = tok[rndn(ntok)];
            size_t at = rndn(o->n + 1), tl = strlen(t);
            buf_add(o, t, tl);
            memmove(o->b + at + tl, o->b + at, o->n - tl - at);
            memcpy(o->b + at, t, tl);
        }
        break;
    case 3: { /* deep nesting */
        static const size_t depths[] = {30, 200, 1000};
        size_t d = depths[rndn(3)];
        int kind = (int)rndn(5);
        static const char *const open_tex[] = {"\\frac{", "{", "\\sqrt{", "x_{", "\\left("};
        static const char *const close_tex[] = {"}{y}", "}", "}", "}", "\\right)"};
        if (mml) {
            buf_str(o, "<math>");
            for (k = 0; k < d; k++) buf_str(o, kind & 1 ? "<mrow>" : "<msqrt>");
            buf_str(o, "<mi>x</mi>");
            for (k = 0; k < d; k++) buf_str(o, kind & 1 ? "</mrow>" : "</msqrt>");
            buf_str(o, "</math>");
        } else {
            for (k = 0; k < d; k++) buf_str(o, open_tex[kind]);
            buf_str(o, "x");
            for (k = 0; k < d; k++) buf_str(o, close_tex[kind]);
        }
        break;
    }
    case 4: /* duplicated chunks */
        for (i = rndn(12) + 2; i--;) buf_add(o, b, n);
        break;
    case 5: { /* macro blow-up: doubling chains, huge bodies */
        size_t m = 4 + rndn(28);
        char l[96];
        if (mml) {
            buf_add(o, b, n);
            break;
        }
        buf_str(o, "\\def\\a0{xy}");
        for (k = 1; k < m; k++) {
            snprintf(l, sizeof l, "\\def\\a%c{\\a%c\\a%c}", (char)('0' + (k % 40)),
                     (char)('0' + ((k - 1) % 40)), (char)('0' + ((k - 1) % 40)));
            buf_str(o, l);
        }
        snprintf(l, sizeof l, "\\a%c", (char)('0' + ((m - 1) % 40)));
        buf_str(o, l);
        break;
    }
    case 6: { /* long text and many terms */
        size_t L = (size_t)1 << (6 + rndn(8));
        buf_str(o, mml ? "<math><mtext>" : "\\text{");
        for (k = 0; k < L; k++) buf_add(o, rndn(8) ? "W" : " ", 1);
        buf_str(o, mml ? "</mtext></math>" : "}");
        if (!mml)
            for (k = 0; k < L / 4; k++) buf_str(o, "+a_{k}^{2}");
        break;
    }
    case 7: /* control bytes and invalid UTF-8 */
        for (i = 0; i < n; i++) {
            buf_add(o, b + i, 1);
            if (rndn(30) == 0) {
                static const char junk[] = "\x01\x7f\xc0\xff\xed\xa0\x80\x00\r\t\x1b";
                buf_add(o, junk + rndn(sizeof junk - 1), 1);
            }
        }
        break;
    case 8: /* extreme numbers */
        for (i = 0; i < n; i++) {
            if (b[i] >= '0' && b[i] <= '9' && rndn(3) == 0) {
                while (i + 1 < n && b[i + 1] >= '0' && b[i + 1] <= '9') i++;
                buf_str(o, NUMS[rndn(sizeof NUMS / sizeof NUMS[0])]);
            } else {
                buf_add(o, b + i, 1);
            }
        }
        break;
    default: /* splice two seeds */
    {
        const Seed *t = &g_seed[rndn((size_t)g_nseed)];
        size_t a = rndn(n + 1), f = rndn(t->n + 1), l = rndn(200) + 1;
        if (f + l > t->n) l = t->n - f;
        buf_add(o, b, a);
        buf_add(o, t->d + f, l);
        buf_add(o, b + a, n - a);
        break;
    }
    }
}

/* TeX seeds: the second string of each ["name", "tex"] pair in
 * formulas.json (JSON escapes \\ and \" undone; the file has no others). */
static void load_tex_seeds(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *b, *p;
    long len;
    if (!f) return;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)len + 1);
    if (!b || fread(b, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(b);
        return;
    }
    fclose(f);
    b[len] = 0;
    p = b;
    while ((p = strchr(p, '[')) != NULL && g_nseed < 512) {
        char name[48], tex[2048];
        size_t k;
        int which;
        p++;
        for (which = 0; which < 2; which++) {
            char *out = which ? tex : name;
            size_t cap = which ? sizeof tex : sizeof name;
            k = 0;
            p = strchr(p, '"');
            if (!p) break;
            p++;
            while (*p && *p != '"') {
                char c = *p++;
                if (c == '\\' && *p) c = *p++;
                if (k + 1 < cap) out[k++] = c;
            }
            out[k] = 0;
            if (*p) p++;
        }
        if (!p) break;
        {
            Seed *s = &g_seed[g_nseed];
            s->n = strlen(tex);
            s->d = malloc(s->n + 1);
            memcpy(s->d, tex, s->n + 1);
            snprintf(s->name, sizeof s->name, "tex:%s", name);
            g_nseed++;
        }
    }
    free(b);
}

/* --math PACK testdata/math [ITERS] [SEED] */
static int mj_main(int argc, char **argv, CrPack *pack)
{
    long iters = argc > 3 ? atol(argv[3]) : 200, i;
    uint32_t budget = getenv("CR_FUZZ_MM_BUDGET_MS") ? (uint32_t)atol(getenv("CR_FUZZ_MM_BUDGET_MS"))
                                                     : 20000u;
    CrAsset *a = cr_pack_find(pack, "js:math");
    const uint8_t *bc = a ? cr_asset_data(a) : NULL;
    CrJsPolicy pol = {(size_t)1 << 30, (size_t)256 << 20, 0, (size_t)16 << 20};
    Buf o = {0};
    double t0 = now_ms();
    char p[2048];
    int ntex;
    long seed_ok = 0, seed_err = 0;
    if (!bc) {
        fprintf(stderr, "cctext-render-fuzz: no js:math in the pack\n");
        return 2;
    }
    g_fpack = pack;
    g_mj = cr_js_new("math", bc, a->raw_len, &pol);
    cr_js_set_loader(g_mj, fz_src_get, fz_src_put, NULL);
    snprintf(p, sizeof p, "%s/formulas.json", argv[2]);
    load_tex_seeds(p);
    ntex = g_nseed;
    g_ext = ".mml";
    snprintf(p, sizeof p, "%s/mml", argv[2]);
    load_dir(p, 1);
    if (!ntex || g_nseed == ntex) {
        fprintf(stderr, "cctext-render-fuzz: no TeX or MathML seeds under %s\n", argv[2]);
        return 2;
    }
    /* Seeds: the valid ones must render (the error cases in
     * formulas.json are CCTEXT_ERR answers). */
    for (i = 0; i < g_nseed; i++)
        mj_one(g_seed[i].name, g_seed[i].d, g_seed[i].n, budget > 60000 ? budget : 60000,
               i >= ntex, 1);
    if (g_mj_ok + g_mj_err < g_nseed || g_mj_ok < g_nseed - 4) {
        fprintf(stderr, "cctext-render-fuzz: only %ld of %d math seeds rendered\n", g_mj_ok,
                g_nseed);
        return 1;
    }
    seed_ok = g_mj_ok;
    seed_err = g_mj_err;
    for (i = 0; i < iters; i++) {
        long si = (long)rndn((size_t)g_nseed);
        const Seed *s = &g_seed[si];
        char what[64];
        int mml = si >= ntex;
        mj_mutate(s, mml, &o);
        snprintf(what, sizeof what, "math #%ld of %s", i, s->name);
        if (getenv("CR_FUZZ_DUMP") && (atol(getenv("CR_FUZZ_DUMP")) == i ||
                                       !strcmp(getenv("CR_FUZZ_DUMP"), "all"))) {
            char dp[64];
            FILE *df;
            snprintf(dp, sizeof dp, "cr-fuzz-%ld.%s", i, mml ? "mml" : "tex");
            df = fopen(dp, "wb");
            if (df) {
                fwrite(o.b ? o.b : "", 1, o.n, df);
                fclose(df);
            }
        }
        if (!getenv("CR_FUZZ_DUMP_ONLY"))
            mj_one(what, o.b ? o.b : "", o.n, budget, mml, (int)rndn(2));
    }
    {
        CrJsStats st;
        cr_js_stats(g_mj, &st);
        printf("cctext-render-fuzz --math: %d seeds, %ld mutants: %ld rendered, %ld errors, %ld "
               "over the node cap, %ld timeouts, %u engine starts, %.1f s; slowest %.0f ms (%s)\n",
               g_nseed, iters, g_mj_ok - seed_ok, g_mj_err - seed_err, g_mj_large, g_mj_timeout,
               st.starts, (now_ms() - t0) / 1000.0, g_worst_ms, g_worst);
    }
    cr_js_stop(g_mj);
    free(g_mj);
    free(o.b);
    return 0;
}

int main(int argc, char **argv)
{
    static CrPack pack;
    FILE *f;
    long len;
    static uint8_t *pb;
    long iters;
    long i;
    Buf o = {0};
    double t0 = now_ms();
    int mermaid = argc > 1 && !strcmp(argv[1], "--mermaid");
    int math = argc > 1 && !strcmp(argv[1], "--math");
    if (mermaid || math) {
        argv++;
        argc--;
    }
    iters = argc > 3 ? atol(argv[3]) : 2000;
    if (argc < 3) {
        fprintf(stderr,
                "usage: cctext-render-fuzz [--mermaid | --math] PACK CORPUS_DIR [ITERS] [SEED]\n");
        return 2;
    }
    if (argc > 4) g_rng ^= (uint64_t)strtoull(argv[4], NULL, 10) * 0x9E3779B97F4A7C15ull;
    f = fopen(argv[1], "rb");
    if (!f || fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "cctext-render-fuzz: cannot read %s\n", argv[1]);
        return 2;
    }
    pb = malloc((size_t)len);
    if (!pb || fread(pb, 1, (size_t)len, f) != (size_t)len || cr_pack_open(&pack, pb, (size_t)len)) {
        fprintf(stderr, "cctext-render-fuzz: bad pack %s\n", argv[1]);
        return 2;
    }
    fclose(f);
    if (fonts_init(&pack) < 1) return 2;
    if (mermaid) return mm_main(argc, argv, &pack);
    if (math) return mj_main(argc, argv, &pack);
    load_dir(argv[2], 0);
    if (!g_nseed) {
        fprintf(stderr, "cctext-render-fuzz: no *.svg under %s\n", argv[2]);
        return 2;
    }
    /* Fixed hostile cases. */
    {
        size_t k;
        o.n = 0;
        buf_str(&o, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\">");
        for (k = 0; k < 40; k++) buf_str(&o, "<svg width=\"99%\" height=\"99%\">");
        buf_str(&o, "<rect width=\"50%\" height=\"50%\"/>");
        for (k = 0; k < 40; k++) buf_str(&o, "</svg>");
        buf_str(&o, "</svg>");
        render_one("nested-svg-40", o.b, o.n);
        if (g_worst_ms > 2000) {
            fprintf(stderr, "cctext-render-fuzz: 40 nested <svg> took %.0f ms (exponential?)\n",
                    g_worst_ms);
            return 1;
        }
        o.n = 0;
        buf_str(&o, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10\" height=\"10\">");
        for (k = 0; k < 100000; k++) buf_str(&o, "<g>");
        buf_str(&o, "<rect width=\"5\" height=\"5\"/>");
        for (k = 0; k < 100000; k++) buf_str(&o, "</g>");
        buf_str(&o, "</svg>");
        render_one("nest-100k", o.b, o.n);
    }
    for (i = 0; i < g_nseed; i++) {
        if (getenv("CR_FUZZ_TRACE")) fprintf(stderr, "%s\n", g_seed[i].name);
        render_one(g_seed[i].name, g_seed[i].d, g_seed[i].n);
    }
    for (i = 0; i < iters; i++) {
        const Seed *s = &g_seed[rndn((size_t)g_nseed)];
        char what[64];
        mutate(s, &o);
        snprintf(what, sizeof what, "#%ld of %s", i, s->name);
        if (getenv("CR_FUZZ_DUMP") && atol(getenv("CR_FUZZ_DUMP")) == i) {
            /* reproduce one mutant: CR_FUZZ_DUMP=N writes it to cr-fuzz-N.svg */
            char p[64];
            FILE *df;
            snprintf(p, sizeof p, "cr-fuzz-%ld.svg", i);
            df = fopen(p, "wb");
            if (df) {
                fwrite(o.b ? o.b : "", 1, o.n, df);
                fclose(df);
            }
        }
        /* CR_FUZZ_TRACE: name each mutant before it runs (to find a crash) */
        if (getenv("CR_FUZZ_TRACE")) fprintf(stderr, "%s\n", what);
        render_one(what, o.b ? o.b : "", o.n);
    }
    printf("cctext-render-fuzz: %d seeds, %ld mutants: %ld parsed, %ld rejected, %.1f s; slowest "
           "%.0f ms (%s)\n",
           g_nseed, iters, g_ok, g_rej, (now_ms() - t0) / 1000.0, g_worst_ms, g_worst);
    free(o.b);
    return 0;
}
