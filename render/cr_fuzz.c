/*
 * cctext-render fuzz driver (docs/images.md, "Renderer"): the SVG backend
 * (lunasvg + plutovg, the same code the helper runs) over mutated inputs,
 * in process, built with ASan + UBSan (scripts/render_build.cch, variant
 * "san" -> bin/cctext-render-fuzz). Any sanitizer report aborts the run.
 *
 *   cctext-render-fuzz PACK CORPUS_DIR [ITERS] [SEED]
 *
 * Every *.svg under CORPUS_DIR (and one level of subdirectories) is a
 * seed; each seed renders once unmutated, then ITERS mutants follow
 * (bit flips, truncation, duplicated chunks, extreme numbers, deep
 * nesting, self / mutual references, splices, attribute garbage, nested
 * percentage <svg>). Fixed hostile cases run first. The sandbox is not
 * involved (the policy has its own tests); this is about memory safety
 * and undefined behaviour in the parser and rasterizer.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

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
        if (l < 5 || strcmp(e->d_name + l - 4, ".svg") != 0 || st.st_size > (1 << 20)) continue;
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
            float sc = 192.0f / (w > h ? w : h);
            uint32_t pw = (uint32_t)(w * sc + 0.999f), ph = (uint32_t)(h * sc + 0.999f);
            uint8_t *px;
            if (pw < 1) pw = 1;
            if (ph < 1) ph = 1;
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
    switch (rndn(13)) {
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
        d = cr_asset_data(a);
        if (d && cr_svg_add_font(fam, bold, italic, d, a->raw_len) == 0) nf++;
    }
    return nf;
}

int main(int argc, char **argv)
{
    static CrPack pack;
    FILE *f;
    long len;
    static uint8_t *pb;
    long iters = argc > 3 ? atol(argv[3]) : 2000;
    long i;
    Buf o = {0};
    double t0 = now_ms();
    if (argc < 3) {
        fprintf(stderr, "usage: cctext-render-fuzz PACK CORPUS_DIR [ITERS] [SEED]\n");
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
    for (i = 0; i < g_nseed; i++) render_one(g_seed[i].name, g_seed[i].d, g_seed[i].n);
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
        render_one(what, o.b ? o.b : "", o.n);
    }
    printf("cctext-render-fuzz: %d seeds, %ld mutants: %ld parsed, %ld rejected, %.1f s; slowest "
           "%.0f ms (%s)\n",
           g_nseed, iters, g_ok, g_rej, (now_ms() - t0) / 1000.0, g_worst_ms, g_worst);
    free(o.b);
    return 0;
}
