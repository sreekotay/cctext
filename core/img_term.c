/*
 * Terminal image encoders (img_term.h, docs/images.md "Terminal").
 * Pure functions over straight RGBA from the loader; no globals but
 * read-only tables built once (a benign race: every thread writes the
 * same values).
 */
#include "img_term.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- sRGB <-> linear --------------------------------------------------- */

/* sRGB 8-bit -> linear (the IEC 61966-2-1 curve); no libm in this build. */
static const float g_lin[256] = {
    0.000000000e+00f, 3.035269835e-04f, 6.070539671e-04f, 9.105809506e-04f, 1.214107934e-03f,
    1.517634918e-03f, 1.821161901e-03f, 2.124688885e-03f, 2.428215868e-03f, 2.731742852e-03f,
    3.035269835e-03f, 3.346535764e-03f, 3.676507324e-03f, 4.024717018e-03f, 4.391442037e-03f,
    4.776953481e-03f, 5.181516702e-03f, 5.605391624e-03f, 6.048833023e-03f, 6.512090793e-03f,
    6.995410187e-03f, 7.499032043e-03f, 8.023192985e-03f, 8.568125618e-03f, 9.134058702e-03f,
    9.721217320e-03f, 1.032982303e-02f, 1.096009401e-02f, 1.161224518e-02f, 1.228648836e-02f,
    1.298303234e-02f, 1.370208305e-02f, 1.444384360e-02f, 1.520851442e-02f, 1.599629337e-02f,
    1.680737575e-02f, 1.764195449e-02f, 1.850022013e-02f, 1.938236096e-02f, 2.028856306e-02f,
    2.121901038e-02f, 2.217388479e-02f, 2.315336618e-02f, 2.415763245e-02f, 2.518685963e-02f,
    2.624122189e-02f, 2.732089164e-02f, 2.842603950e-02f, 2.955683444e-02f, 3.071344373e-02f,
    3.189603307e-02f, 3.310476657e-02f, 3.433980681e-02f, 3.560131488e-02f, 3.688945040e-02f,
    3.820437160e-02f, 3.954623528e-02f, 4.091519691e-02f, 4.231141062e-02f, 4.373502926e-02f,
    4.518620439e-02f, 4.666508634e-02f, 4.817182423e-02f, 4.970656598e-02f, 5.126945837e-02f,
    5.286064702e-02f, 5.448027644e-02f, 5.612849005e-02f, 5.780543019e-02f, 5.951123816e-02f,
    6.124605423e-02f, 6.301001765e-02f, 6.480326669e-02f, 6.662593864e-02f, 6.847816984e-02f,
    7.036009570e-02f, 7.227185068e-02f, 7.421356838e-02f, 7.618538148e-02f, 7.818742181e-02f,
    8.021982031e-02f, 8.228270713e-02f, 8.437621154e-02f, 8.650046204e-02f, 8.865558629e-02f,
    9.084171118e-02f, 9.305896285e-02f, 9.530746663e-02f, 9.758734714e-02f, 9.989872825e-02f,
    1.022417331e-01f, 1.046164841e-01f, 1.070231030e-01f, 1.094617108e-01f, 1.119324278e-01f,
    1.144353738e-01f, 1.169706678e-01f, 1.195384280e-01f, 1.221387722e-01f, 1.247718176e-01f,
    1.274376804e-01f, 1.301364767e-01f, 1.328683216e-01f, 1.356333297e-01f, 1.384316150e-01f,
    1.412632911e-01f, 1.441284709e-01f, 1.470272665e-01f, 1.499597898e-01f, 1.529261520e-01f,
    1.559264637e-01f, 1.589608351e-01f, 1.620293756e-01f, 1.651321945e-01f, 1.682694002e-01f,
    1.714411007e-01f, 1.746474037e-01f, 1.778884160e-01f, 1.811642442e-01f, 1.844749945e-01f,
    1.878207723e-01f, 1.912016827e-01f, 1.946178304e-01f, 1.980693196e-01f, 2.015562538e-01f,
    2.050787364e-01f, 2.086368701e-01f, 2.122307574e-01f, 2.158605001e-01f, 2.195261997e-01f,
    2.232279573e-01f, 2.269658735e-01f, 2.307400485e-01f, 2.345505822e-01f, 2.383975738e-01f,
    2.422811225e-01f, 2.462013267e-01f, 2.501582847e-01f, 2.541520943e-01f, 2.581828529e-01f,
    2.622506575e-01f, 2.663556048e-01f, 2.704977910e-01f, 2.746773121e-01f, 2.788942635e-01f,
    2.831487404e-01f, 2.874408377e-01f, 2.917706498e-01f, 2.961382708e-01f, 3.005437944e-01f,
    3.049873141e-01f, 3.094689228e-01f, 3.139887134e-01f, 3.185467781e-01f, 3.231432091e-01f,
    3.277780981e-01f, 3.324515363e-01f, 3.371636150e-01f, 3.419144249e-01f, 3.467040564e-01f,
    3.515325995e-01f, 3.564001441e-01f, 3.613067798e-01f, 3.662525956e-01f, 3.712376805e-01f,
    3.762621230e-01f, 3.813260114e-01f, 3.864294338e-01f, 3.915724777e-01f, 3.967552307e-01f,
    4.019777798e-01f, 4.072402119e-01f, 4.125426135e-01f, 4.178850708e-01f, 4.232676700e-01f,
    4.286904966e-01f, 4.341536362e-01f, 4.396571738e-01f, 4.452011945e-01f, 4.507857828e-01f,
    4.564110232e-01f, 4.620769997e-01f, 4.677837961e-01f, 4.735314961e-01f, 4.793201831e-01f,
    4.851499401e-01f, 4.910208498e-01f, 4.969329951e-01f, 5.028864580e-01f, 5.088813209e-01f,
    5.149176654e-01f, 5.209955732e-01f, 5.271151257e-01f, 5.332764040e-01f, 5.394794890e-01f,
    5.457244614e-01f, 5.520114015e-01f, 5.583403896e-01f, 5.647115057e-01f, 5.711248295e-01f,
    5.775804404e-01f, 5.840784179e-01f, 5.906188409e-01f, 5.972017884e-01f, 6.038273389e-01f,
    6.104955708e-01f, 6.172065624e-01f, 6.239603917e-01f, 6.307571363e-01f, 6.375968740e-01f,
    6.444796820e-01f, 6.514056374e-01f, 6.583748173e-01f, 6.653872983e-01f, 6.724431570e-01f,
    6.795424696e-01f, 6.866853124e-01f, 6.938717613e-01f, 7.011018919e-01f, 7.083757799e-01f,
    7.156935005e-01f, 7.230551289e-01f, 7.304607401e-01f, 7.379104088e-01f, 7.454042095e-01f,
    7.529422168e-01f, 7.605245047e-01f, 7.681511472e-01f, 7.758222183e-01f, 7.835377915e-01f,
    7.912979403e-01f, 7.991027380e-01f, 8.069522577e-01f, 8.148465722e-01f, 8.227857544e-01f,
    8.307698768e-01f, 8.387990117e-01f, 8.468732315e-01f, 8.549926081e-01f, 8.631572135e-01f,
    8.713671192e-01f, 8.796223969e-01f, 8.879231179e-01f, 8.962693534e-01f, 9.046611744e-01f,
    9.130986518e-01f, 9.215818563e-01f, 9.301108584e-01f, 9.386857285e-01f, 9.473065367e-01f,
    9.559733532e-01f, 9.646862479e-01f, 9.734452904e-01f, 9.822505503e-01f, 9.911020971e-01f,
    1.000000000e+00f
};
static uint8_t g_enc[4097];
static volatile int g_lut_ok;

/* linear -> sRGB: the nearest 8-bit code, by bisection of g_lin. */
static void ti_lut(void) {
    int i;
    if (g_lut_ok) return;
    for (i = 0; i <= 4096; i++) {
        float l = (float)i / 4096.0f;
        int lo = 0, hi = 255;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (g_lin[mid] < l) lo = mid + 1;
            else hi = mid;
        }
        if (lo > 0 && l - g_lin[lo - 1] < g_lin[lo] - l) lo--;
        g_enc[i] = (uint8_t)lo;
    }
    g_lut_ok = 1;
}

static inline uint8_t ti_to_srgb(float l) {
    int k;
    if (l <= 0) return 0;
    if (l >= 1) return 255;
    k = (int)(l * 4096.0f + 0.5f);
    return g_enc[k];
}

static inline int ti_round(float x) {
    return (int)(x >= 0 ? x + 0.5f : x - 0.5f);
}

/* ---- resample ----------------------------------------------------------- */

typedef struct {
    uint32_t i0, n, w0; /* first source index, count, offset into weights */
} TiSpan;

/* Spans of target [c0, c0 + cn) over a source of sn mapped onto tn. */
static int ti_spans(uint32_t sn, uint32_t tn, uint32_t c0, uint32_t cn, TiSpan **spp,
                    float **wp) {
    TiSpan *sp = (TiSpan *)calloc(cn ? cn : 1, sizeof *sp);
    size_t cap = (size_t)cn * ((size_t)(sn / (tn ? tn : 1)) + 3) + 4, nw = 0;
    float *w = (float *)malloc(cap * sizeof *w);
    uint32_t o;
    if (!sp || !w) {
        free(sp);
        free(w);
        return 0;
    }
    for (o = 0; o < cn; o++) {
        uint32_t t = c0 + o;
        if (sn <= tn) {
            uint32_t i = (uint32_t)(((uint64_t)t * 2 + 1) * sn / ((uint64_t)tn * 2));
            if (i >= sn) i = sn - 1;
            sp[o].i0 = i;
            sp[o].n = 1;
            sp[o].w0 = (uint32_t)nw;
            w[nw++] = 1.0f;
        } else {
            double s0 = (double)t * sn / tn, s1 = (double)(t + 1) * sn / tn, tot = s1 - s0;
            uint32_t i = (uint32_t)s0, e = (uint32_t)s1 + ((double)(uint32_t)s1 < s1);
            if (e > sn) e = sn;
            if (i >= e) i = e - 1;
            sp[o].i0 = i;
            sp[o].n = 0;
            sp[o].w0 = (uint32_t)nw;
            for (; i < e && nw < cap; i++) {
                double a = (double)i > s0 ? (double)i : s0;
                double b = (double)(i + 1) < s1 ? (double)(i + 1) : s1;
                w[nw++] = (float)((b - a) / tot);
                sp[o].n++;
            }
        }
    }
    *spp = sp;
    *wp = w;
    return 1;
}

int rtx_timg_scale(const uint8_t *src, uint32_t w, uint32_t h, uint32_t stride,
                   uint32_t tw, uint32_t th, uint32_t cx, uint32_t cy, uint32_t cw,
                   uint32_t ch, uint8_t *dst) {
    TiSpan *sx = NULL, *sy = NULL;
    float *wx = NULL, *wy = NULL, *row[2] = {NULL, NULL}, *acc = NULL;
    int64_t tag[2] = {-1, -1};
    uint32_t oy, ox, k;
    int ok = 0;
    if (!src || !dst || !w || !h || !tw || !th || !cw || !ch || cx + cw > tw || cy + ch > th)
        return 0;
    ti_lut();
    if (!ti_spans(w, tw, cx, cw, &sx, &wx) || !ti_spans(h, th, cy, ch, &sy, &wy)) goto out;
    row[0] = (float *)malloc((size_t)cw * 4 * sizeof(float));
    row[1] = (float *)malloc((size_t)cw * 4 * sizeof(float));
    acc = (float *)malloc((size_t)cw * 4 * sizeof(float));
    if (!row[0] || !row[1] || !acc) goto out;
    for (oy = 0; oy < ch; oy++) {
        memset(acc, 0, (size_t)cw * 4 * sizeof(float));
        for (k = 0; k < sy[oy].n; k++) {
            uint32_t iy = sy[oy].i0 + k;
            float wyk = wy[sy[oy].w0 + k];
            int slot = (int)(iy & 1);
            float *hr = row[slot];
            if (tag[slot] != (int64_t)iy) {
                const uint8_t *s = src + (size_t)iy * stride;
                for (ox = 0; ox < cw; ox++) {
                    float r = 0, g = 0, b = 0, a = 0;
                    uint32_t j;
                    for (j = 0; j < sx[ox].n; j++) {
                        const uint8_t *p = s + (size_t)(sx[ox].i0 + j) * 4;
                        float pa = p[3] * (1.0f / 255.0f) * wx[sx[ox].w0 + j];
                        r += g_lin[p[0]] * pa;
                        g += g_lin[p[1]] * pa;
                        b += g_lin[p[2]] * pa;
                        a += pa;
                    }
                    hr[ox * 4 + 0] = r;
                    hr[ox * 4 + 1] = g;
                    hr[ox * 4 + 2] = b;
                    hr[ox * 4 + 3] = a;
                }
                tag[slot] = (int64_t)iy;
            }
            for (ox = 0; ox < cw * 4; ox++) acc[ox] += hr[ox] * wyk;
        }
        for (ox = 0; ox < cw; ox++) {
            uint8_t *d = dst + ((size_t)oy * cw + ox) * 4;
            float a = acc[ox * 4 + 3];
            if (a > 1e-6f) {
                float ia = 1.0f / a;
                d[0] = ti_to_srgb(acc[ox * 4 + 0] * ia);
                d[1] = ti_to_srgb(acc[ox * 4 + 1] * ia);
                d[2] = ti_to_srgb(acc[ox * 4 + 2] * ia);
                d[3] = (uint8_t)(a >= 1.0f ? 255 : (int)(a * 255.0f + 0.5f));
            } else {
                d[0] = d[1] = d[2] = d[3] = 0;
            }
        }
    }
    ok = 1;
out:
    free(sx);
    free(sy);
    free(wx);
    free(wy);
    free(row[0]);
    free(row[1]);
    free(acc);
    return ok;
}

/* ---- the xterm 256-colour palette -------------------------------------- */

static const uint8_t g_ansi16[16][3] = {
    {0, 0, 0},       {205, 0, 0},   {0, 205, 0},   {205, 205, 0},
    {0, 0, 238},     {205, 0, 205}, {0, 205, 205}, {229, 229, 229},
    {127, 127, 127}, {255, 0, 0},   {0, 255, 0},   {255, 255, 0},
    {92, 92, 255},   {255, 0, 255}, {0, 255, 255}, {255, 255, 255}};
static const uint8_t g_cube[6] = {0, 95, 135, 175, 215, 255};

void rtx_timg_256_rgb(int i, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (i < 0) i = 0;
    if (i > 255) i = 255;
    if (i < 16) {
        *r = g_ansi16[i][0];
        *g = g_ansi16[i][1];
        *b = g_ansi16[i][2];
    } else if (i < 232) {
        int k = i - 16;
        *r = g_cube[k / 36];
        *g = g_cube[(k / 6) % 6];
        *b = g_cube[k % 6];
    } else {
        uint8_t v = (uint8_t)(8 + 10 * (i - 232));
        *r = *g = *b = v;
    }
}

static inline int ti_cube_i(int v) {
    return v < 48 ? 0 : v < 115 ? 1 : (v - 35) / 40;
}

static inline int ti_d2(int r0, int g0, int b0, int r1, int g1, int b1) {
    int dr = r0 - r1, dg = g0 - g1, db = b0 - b1;
    return dr * dr * 3 + dg * dg * 4 + db * db * 2;
}

int rtx_timg_256(uint8_t r, uint8_t g, uint8_t b) {
    int ri = ti_cube_i(r), gi = ti_cube_i(g), bi = ti_cube_i(b);
    int cube = 16 + 36 * ri + 6 * gi + bi;
    int dc = ti_d2(r, g, b, g_cube[ri], g_cube[gi], g_cube[bi]);
    int avg = (r + g + b) / 3, k = (avg - 3) / 10, gv, dg;
    if (k < 0) k = 0;
    if (k > 23) k = 23;
    gv = 8 + 10 * k;
    dg = ti_d2(r, g, b, gv, gv, gv);
    return dg < dc ? 232 + k : cube;
}

/* ---- utf-8 / SGR -------------------------------------------------------- */

int rtx_timg_utf8(uint32_t cp, char *o) {
    if (cp < 0x80) {
        o[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        o[0] = (char)(0xC0 | (cp >> 6));
        o[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12));
        o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | (cp >> 18));
    o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static int ti_sgr_one(int32_t c, int base, char *o, size_t cap) {
    if (c < 0) return 0;
    if (RTX_TIMG_IS_RGB(c))
        return snprintf(o, cap, ";%d;2;%d;%d;%d", base, (int)((c >> 16) & 255),
                        (int)((c >> 8) & 255), (int)(c & 255));
    return snprintf(o, cap, ";%d;5;%d", base, (int)c);
}

int rtx_timg_sgr(int32_t fg, int32_t bg, char *out, size_t cap) {
    int n = 0, k;
    if (cap < 48) return 0;
    out[n++] = '\x1b';
    out[n++] = '[';
    out[n++] = '0';
    k = ti_sgr_one(fg, 38, out + n, cap - (size_t)n);
    if (k > 0) n += k;
    k = ti_sgr_one(bg, 48, out + n, cap - (size_t)n);
    if (k > 0) n += k;
    out[n++] = 'm';
    out[n] = 0;
    return n;
}

/* ---- block art ---------------------------------------------------------- */

static const uint32_t g_quad[16] = {0x20,   0x2598, 0x259D, 0x2580, 0x2596, 0x258C,
                                    0x259E, 0x259B, 0x2597, 0x259A, 0x2590, 0x259C,
                                    0x2584, 0x2599, 0x259F, 0x2588};

/* pat: bit k = sub-pixel k (row-major) shows fg. */
static uint32_t ti_glyph(int glyphs, unsigned pat) {
    if (glyphs == RTX_TIMG_HALF)
        return pat == 0 ? 0x20 : pat == 1 ? 0x2580 : pat == 2 ? 0x2584 : 0x2588;
    if (glyphs == RTX_TIMG_QUAD) return g_quad[pat & 15];
    if (pat == 0) return 0x20;
    if (pat == 63) return 0x2588;
    if (pat == 21) return 0x258C;
    if (pat == 42) return 0x2590;
    return 0x1FB00 + pat - 1 - (pat > 21) - (pat > 42);
}

static int32_t ti_color(int colors, int r, int g, int b) {
    if (colors == RTX_TIMG_256) return rtx_timg_256((uint8_t)r, (uint8_t)g, (uint8_t)b);
    return RTX_TIMG_RGB(r, g, b);
}

static inline int ti_clamp8(int v) {
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

/* Floyd-Steinberg to the 256 palette over the opaque sub-pixels. */
static void ti_quant256(uint8_t *grid, uint32_t gw, uint32_t gh, int dither) {
    float *err = dither ? (float *)calloc((size_t)(gw + 2) * 2 * 3, sizeof(float)) : NULL;
    uint32_t x, y;
    for (y = 0; y < gh; y++) {
        float *cur = err ? err + (size_t)(y & 1) * (gw + 2) * 3 : NULL;
        float *nxt = err ? err + (size_t)((y + 1) & 1) * (gw + 2) * 3 : NULL;
        if (nxt) memset(nxt, 0, (size_t)(gw + 2) * 3 * sizeof(float));
        for (x = 0; x < gw; x++) {
            uint8_t *p = grid + ((size_t)y * gw + x) * 4;
            int r = p[0], g = p[1], b = p[2], q, c;
            uint8_t qc[3];
            if (p[3] < 128) continue;
            if (cur) {
                r = ti_clamp8(r + (int)ti_round(cur[(x + 1) * 3 + 0]));
                g = ti_clamp8(g + (int)ti_round(cur[(x + 1) * 3 + 1]));
                b = ti_clamp8(b + (int)ti_round(cur[(x + 1) * 3 + 2]));
            }
            q = rtx_timg_256((uint8_t)r, (uint8_t)g, (uint8_t)b);
            rtx_timg_256_rgb(q, &qc[0], &qc[1], &qc[2]);
            if (cur) {
                int e[3];
                e[0] = r - qc[0];
                e[1] = g - qc[1];
                e[2] = b - qc[2];
                for (c = 0; c < 3; c++) {
                    cur[(x + 2) * 3 + c] += (float)e[c] * 7.0f / 16.0f;
                    nxt[x * 3 + c] += (float)e[c] * 3.0f / 16.0f;
                    nxt[(x + 1) * 3 + c] += (float)e[c] * 5.0f / 16.0f;
                    nxt[(x + 2) * 3 + c] += (float)e[c] / 16.0f;
                }
            }
            p[0] = qc[0];
            p[1] = qc[1];
            p[2] = qc[2];
        }
    }
    free(err);
}

/* One cell from its n sub-pixels. */
static void ti_cell(int glyphs, int colors, int n, const int *pr, const int *pg,
                    const int *pb, const int *clear, RtxTimgCell *c) {
    int k, nclear = 0;
    unsigned best = 0, m, lim;
    double bestv = -1;
    long s1[3], s0[3];
    int n1, n0;
    c->fg = -1;
    c->bg = -1;
    c->cp = 0x20;
    for (k = 0; k < n; k++) nclear += clear[k];
    if (nclear == n) return;
    if (nclear) {
        /* The opaque part in fg over the terminal's own background. */
        for (k = 0; k < n; k++)
            if (!clear[k]) best |= 1u << k;
    } else {
        int same = 1;
        for (k = 1; k < n; k++)
            if (pr[k] != pr[0] || pg[k] != pg[0] || pb[k] != pb[0]) same = 0;
        if (same) {
            c->bg = ti_color(colors, pr[0], pg[0], pb[0]);
            return;
        }
        /* The best split in two (least squared error): maximise
         * |S1|^2 / n1 + |S0|^2 / n0. The last sub-pixel stays in bg. */
        lim = 1u << (n - 1);
        for (m = 1; m < lim; m++) {
            double v;
            s1[0] = s1[1] = s1[2] = s0[0] = s0[1] = s0[2] = 0;
            n1 = n0 = 0;
            for (k = 0; k < n; k++) {
                long *s = (m & (1u << k)) ? s1 : s0;
                s[0] += pr[k];
                s[1] += pg[k];
                s[2] += pb[k];
                if (m & (1u << k)) n1++;
                else n0++;
            }
            v = (double)(s1[0] * s1[0] + s1[1] * s1[1] + s1[2] * s1[2]) / n1 +
                (double)(s0[0] * s0[0] + s0[1] * s0[1] + s0[2] * s0[2]) / n0;
            if (v > bestv) {
                bestv = v;
                best = m;
            }
        }
    }
    {
        /* Each side's colour: the mean in linear light. */
        float f1[3] = {0, 0, 0}, f0[3] = {0, 0, 0};
        n1 = n0 = 0;
        for (k = 0; k < n; k++) {
            float *f;
            if (best & (1u << k)) {
                f = f1;
                n1++;
            } else if (!clear[k]) {
                f = f0;
                n0++;
            } else {
                continue;
            }
            f[0] += g_lin[pr[k]];
            f[1] += g_lin[pg[k]];
            f[2] += g_lin[pb[k]];
        }
        ti_lut();
        c->cp = ti_glyph(glyphs, best);
        c->fg = ti_color(colors, ti_to_srgb(f1[0] / (float)n1), ti_to_srgb(f1[1] / (float)n1),
                         ti_to_srgb(f1[2] / (float)n1));
        if (n0)
            c->bg = ti_color(colors, ti_to_srgb(f0[0] / (float)n0),
                             ti_to_srgb(f0[1] / (float)n0), ti_to_srgb(f0[2] / (float)n0));
    }
    if (n0 && c->fg == c->bg) {
        c->cp = 0x20;
        c->fg = -1;
    }
}

void rtx_timg_matte(uint8_t *rgba, uint32_t w, uint32_t h, uint32_t stride, uint32_t bg) {
    unsigned br = (bg >> 16) & 255u, bgc = (bg >> 8) & 255u, bb = bg & 255u;
    uint32_t x, y;
    if (!rgba) return;
    for (y = 0; y < h; y++) {
        uint8_t *p = rgba + (size_t)y * stride;
        for (x = 0; x < w; x++, p += 4) {
            unsigned a = p[3], ia;
            if (a == 0 || a == 255) continue;
            ia = 255u - a;
            p[0] = (uint8_t)((p[0] * a + br * ia + 127u) / 255u);
            p[1] = (uint8_t)((p[1] * a + bgc * ia + 127u) / 255u);
            p[2] = (uint8_t)((p[2] * a + bb * ia + 127u) / 255u);
        }
    }
}

int rtx_timg_blocks(const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t stride,
                    uint32_t disp_w, uint32_t disp_h, uint32_t cell_w, uint32_t cell_h,
                    uint32_t cols, uint32_t rows, int glyphs, int colors, int dither,
                    RtxTimgCell *out) {
    uint32_t sx = glyphs == RTX_TIMG_HALF ? 1 : 2;
    uint32_t sy = glyphs == RTX_TIMG_SEXT ? 3 : 2;
    uint32_t gw = cols * sx, gh = rows * sy, tw, th, x, y;
    uint8_t *sub = NULL, *grid = NULL;
    int ok = 0;
    if (!rgba || !out || !w || !h || !cols || !rows || !cell_w || !cell_h) return 0;
    if (!disp_w || !disp_h) {
        disp_w = w;
        disp_h = h;
    }
    tw = (uint32_t)(((uint64_t)disp_w * sx * 2 + cell_w) / ((uint64_t)cell_w * 2));
    th = (uint32_t)(((uint64_t)disp_h * sy * 2 + cell_h) / ((uint64_t)cell_h * 2));
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    if (tw > gw) tw = gw;
    if (th > gh) th = gh;
    sub = (uint8_t *)malloc((size_t)tw * th * 4);
    grid = (uint8_t *)calloc((size_t)gw * gh, 4);
    if (!sub || !grid) goto out;
    if (!rtx_timg_scale(rgba, w, h, stride, tw, th, 0, 0, tw, th, sub)) goto out;
    for (y = 0; y < th; y++)
        memcpy(grid + (size_t)y * gw * 4, sub + (size_t)y * tw * 4, (size_t)tw * 4);
    if (colors == RTX_TIMG_256) ti_quant256(grid, gw, gh, dither);
    for (y = 0; y < rows; y++) {
        for (x = 0; x < cols; x++) {
            int pr[6], pg[6], pb[6], clear[6], k, n = (int)(sx * sy);
            for (k = 0; k < n; k++) {
                uint32_t gx = x * sx + (uint32_t)k % sx, gy = y * sy + (uint32_t)k / sx;
                const uint8_t *p = grid + ((size_t)gy * gw + gx) * 4;
                pr[k] = p[0];
                pg[k] = p[1];
                pb[k] = p[2];
                clear[k] = p[3] < 128;
            }
            ti_cell(glyphs, colors, n, pr, pg, pb, clear, &out[(size_t)y * cols + x]);
        }
    }
    ok = 1;
out:
    free(sub);
    free(grid);
    return ok;
}

/* ---- growable byte buffer ------------------------------------------------ */

typedef struct {
    uint8_t *p;
    size_t n, cap;
    int bad;
} TiBuf;

static int ti_grow(TiBuf *b, size_t more) {
    size_t cap;
    uint8_t *p;
    if (b->bad) return 0;
    if (b->n + more <= b->cap) return 1;
    cap = b->cap ? b->cap : 4096;
    while (cap < b->n + more) cap *= 2;
    p = (uint8_t *)realloc(b->p, cap);
    if (!p) {
        b->bad = 1;
        return 0;
    }
    b->p = p;
    b->cap = cap;
    return 1;
}

static void ti_put(TiBuf *b, const void *s, size_t n) {
    if (!ti_grow(b, n)) return;
    memcpy(b->p + b->n, s, n);
    b->n += n;
}

static void ti_putc(TiBuf *b, uint8_t c) {
    if (!ti_grow(b, 1)) return;
    b->p[b->n++] = c;
}

static void ti_puti(TiBuf *b, unsigned v) {
    char t[16];
    int k = snprintf(t, sizeof t, "%u", v);
    ti_put(b, t, (size_t)k);
}

/* ---- sixel -------------------------------------------------------------- */

typedef struct {
    uint8_t lo[3], hi[3];
    uint64_t count;
} TiBox;

#define TI_BIN(r, g, b) ((((uint32_t)(r) >> 3) << 10) | (((uint32_t)(g) >> 3) << 5) | ((uint32_t)(b) >> 3))

/* Shrink a box to the bins it holds; 0 when empty. */
static int ti_box_fit(TiBox *bx, const uint32_t *hist) {
    uint8_t lo[3] = {31, 31, 31}, hi[3] = {0, 0, 0};
    uint64_t cnt = 0;
    int r, g, b;
    for (r = bx->lo[0]; r <= bx->hi[0]; r++)
        for (g = bx->lo[1]; g <= bx->hi[1]; g++)
            for (b = bx->lo[2]; b <= bx->hi[2]; b++) {
                uint32_t c = hist[(r << 10) | (g << 5) | b];
                if (!c) continue;
                cnt += c;
                if (r < lo[0]) lo[0] = (uint8_t)r;
                if (g < lo[1]) lo[1] = (uint8_t)g;
                if (b < lo[2]) lo[2] = (uint8_t)b;
                if (r > hi[0]) hi[0] = (uint8_t)r;
                if (g > hi[1]) hi[1] = (uint8_t)g;
                if (b > hi[2]) hi[2] = (uint8_t)b;
            }
    bx->count = cnt;
    if (!cnt) return 0;
    memcpy(bx->lo, lo, 3);
    memcpy(bx->hi, hi, 3);
    return 1;
}

/* Median cut over a 15-bit histogram: at most ncolors boxes. */
static int ti_median_cut(const uint32_t *hist, const uint64_t (*sum)[3], int ncolors,
                         uint8_t (*pal)[3]) {
    TiBox box[256];
    int nb = 0, i;
    box[0].lo[0] = box[0].lo[1] = box[0].lo[2] = 0;
    box[0].hi[0] = box[0].hi[1] = box[0].hi[2] = 31;
    if (!ti_box_fit(&box[0], hist)) return 0;
    nb = 1;
    while (nb < ncolors) {
        int pick = -1, axis = 0, a;
        uint64_t score = 0;
        for (i = 0; i < nb; i++) {
            int best_ax = 0, span = 0;
            for (a = 0; a < 3; a++) {
                int s = box[i].hi[a] - box[i].lo[a];
                if (s > span) {
                    span = s;
                    best_ax = a;
                }
            }
            if (span == 0) continue;
            if (pick < 0 || (uint64_t)span * box[i].count > score) {
                score = (uint64_t)span * box[i].count;
                pick = i;
                axis = best_ax;
            }
        }
        if (pick < 0) break;
        {
            /* Split at the weighted median along `axis`. */
            TiBox *bx = &box[pick], nw;
            uint64_t half = bx->count / 2, acc = 0;
            int cut = bx->lo[axis], v;
            for (v = bx->lo[axis]; v < bx->hi[axis]; v++) {
                int r, g, b;
                uint8_t lo[3], hi[3];
                memcpy(lo, bx->lo, 3);
                memcpy(hi, bx->hi, 3);
                lo[axis] = hi[axis] = (uint8_t)v;
                for (r = lo[0]; r <= hi[0]; r++)
                    for (g = lo[1]; g <= hi[1]; g++)
                        for (b = lo[2]; b <= hi[2]; b++) acc += hist[(r << 10) | (g << 5) | b];
                cut = v;
                if (acc >= half) break;
            }
            nw = *bx;
            nw.lo[axis] = (uint8_t)(cut + 1);
            bx->hi[axis] = (uint8_t)cut;
            ti_box_fit(bx, hist);
            if (ti_box_fit(&nw, hist)) box[nb++] = nw;
        }
    }
    for (i = 0; i < nb; i++) {
        uint64_t s[3] = {0, 0, 0}, cnt = 0;
        int r, g, b;
        for (r = box[i].lo[0]; r <= box[i].hi[0]; r++)
            for (g = box[i].lo[1]; g <= box[i].hi[1]; g++)
                for (b = box[i].lo[2]; b <= box[i].hi[2]; b++) {
                    uint32_t k = (uint32_t)((r << 10) | (g << 5) | b);
                    if (!hist[k]) continue;
                    s[0] += sum[k][0];
                    s[1] += sum[k][1];
                    s[2] += sum[k][2];
                    cnt += hist[k];
                }
        if (!cnt) cnt = 1;
        pal[i][0] = (uint8_t)((s[0] + cnt / 2) / cnt);
        pal[i][1] = (uint8_t)((s[1] + cnt / 2) / cnt);
        pal[i][2] = (uint8_t)((s[2] + cnt / 2) / cnt);
    }
    return nb;
}

int rtx_timg_sixel(const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t stride,
                   int ncolors, const volatile int *cancel, char **out, size_t *on) {
    uint32_t *hist = NULL;
    uint64_t(*sum)[3] = NULL;
    int16_t *map = NULL;
    uint8_t *idx = NULL, pal[256][3], *bits = NULL;
    int npal, i;
    uint32_t x, y;
    TiBuf B = {0};
    int ok = 0;
    if (out) *out = NULL;
    if (on) *on = 0;
    if (!rgba || !w || !h || !out || !on) return 0;
    if (ncolors < 2) ncolors = 2;
    if (ncolors > 256) ncolors = 256;
    hist = (uint32_t *)calloc(32768, sizeof *hist);
    sum = (uint64_t(*)[3])calloc(32768, sizeof *sum);
    map = (int16_t *)malloc(32768 * sizeof *map);
    idx = (uint8_t *)malloc((size_t)w * h);
    bits = (uint8_t *)malloc((size_t)256 * w);
    if (!hist || !sum || !map || !idx || !bits) goto out;
    for (y = 0; y < h; y++) {
        const uint8_t *s = rgba + (size_t)y * stride;
        for (x = 0; x < w; x++, s += 4) {
            uint32_t k;
            if (s[3] < 128) continue;
            k = TI_BIN(s[0], s[1], s[2]);
            hist[k]++;
            sum[k][0] += s[0];
            sum[k][1] += s[1];
            sum[k][2] += s[2];
        }
    }
    npal = ti_median_cut(hist, (const uint64_t(*)[3])sum, ncolors, pal);
    for (i = 0; i < 32768; i++) map[i] = -1;
    for (y = 0; y < h; y++) {
        const uint8_t *s = rgba + (size_t)y * stride;
        for (x = 0; x < w; x++, s += 4) {
            uint32_t k;
            int bi = 0, bd = -1, p;
            if (s[3] < 128 || !npal) {
                idx[(size_t)y * w + x] = 255;
                continue;
            }
            k = TI_BIN(s[0], s[1], s[2]);
            if (map[k] < 0) {
                int cr = (int)((sum[k][0] + hist[k] / 2) / hist[k]);
                int cg = (int)((sum[k][1] + hist[k] / 2) / hist[k]);
                int cb = (int)((sum[k][2] + hist[k] / 2) / hist[k]);
                for (p = 0; p < npal; p++) {
                    int d = ti_d2(cr, cg, cb, pal[p][0], pal[p][1], pal[p][2]);
                    if (bd < 0 || d < bd) {
                        bd = d;
                        bi = p;
                    }
                }
                map[k] = (int16_t)bi;
            }
            idx[(size_t)y * w + x] = (uint8_t)map[k];
        }
    }
    /* A transparent pixel is index 255; keep 255 free when it is used. */
    if (npal == 256) {
        /* Rare: fold the last colour into its nearest neighbour. */
        int nb = 0, bd = -1;
        for (i = 0; i < 255; i++) {
            int d = ti_d2(pal[255][0], pal[255][1], pal[255][2], pal[i][0], pal[i][1], pal[i][2]);
            if (bd < 0 || d < bd) {
                bd = d;
                nb = i;
            }
        }
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                uint8_t *q = &idx[(size_t)y * w + x];
                const uint8_t *s = rgba + (size_t)y * stride + (size_t)x * 4;
                if (*q == 255 && s[3] >= 128) *q = (uint8_t)nb;
            }
        npal = 255;
    }
    ti_put(&B, "\x1bP0;1;0q\"1;1;", 13);
    ti_puti(&B, w);
    ti_putc(&B, ';');
    ti_puti(&B, h);
    for (i = 0; i < npal; i++) {
        ti_putc(&B, '#');
        ti_puti(&B, (unsigned)i);
        ti_put(&B, ";2;", 3);
        ti_puti(&B, (unsigned)((pal[i][0] * 100 + 127) / 255));
        ti_putc(&B, ';');
        ti_puti(&B, (unsigned)((pal[i][1] * 100 + 127) / 255));
        ti_putc(&B, ';');
        ti_puti(&B, (unsigned)((pal[i][2] * 100 + 127) / 255));
    }
    for (y = 0; y < h; y += 6) {
        uint32_t lo[256], hi[256];
        uint8_t used[256];
        int first = 1, r;
        if (cancel && *cancel) goto out;
        memset(used, 0, sizeof used);
        for (r = 0; r < 6 && y + (uint32_t)r < h; r++) {
            const uint8_t *q = idx + (size_t)(y + (uint32_t)r) * w;
            for (x = 0; x < w; x++) {
                uint8_t c = q[x];
                if (c == 255) continue;
                if (!used[c]) {
                    used[c] = 1;
                    lo[c] = hi[c] = x;
                    memset(bits + (size_t)c * w, 0, w);
                }
                if (x < lo[c]) lo[c] = x;
                if (x > hi[c]) hi[c] = x;
                bits[(size_t)c * w + x] |= (uint8_t)(1u << r);
            }
        }
        for (i = 0; i < npal; i++) {
            const uint8_t *bb;
            uint32_t run = 0;
            int last = -1;
            if (!used[i]) continue;
            if (!first) ti_putc(&B, '$');
            first = 0;
            ti_putc(&B, '#');
            ti_puti(&B, (unsigned)i);
            bb = bits + (size_t)i * w;
            if (lo[i] > 0) {
                /* Skip to the first column with empty sixels. */
                if (lo[i] > 3) {
                    ti_putc(&B, '!');
                    ti_puti(&B, lo[i]);
                    ti_putc(&B, '?');
                } else {
                    for (x = 0; x < lo[i]; x++) ti_putc(&B, '?');
                }
            }
            for (x = lo[i]; x <= hi[i] + 1; x++) {
                int ch = x <= hi[i] ? 63 + bb[x] : -2;
                if (ch == last) {
                    run++;
                    continue;
                }
                if (last >= 0 && run) {
                    if (run > 3) {
                        ti_putc(&B, '!');
                        ti_puti(&B, run);
                        ti_putc(&B, (uint8_t)last);
                    } else {
                        while (run--) ti_putc(&B, (uint8_t)last);
                    }
                }
                last = ch;
                run = 1;
            }
        }
        ti_putc(&B, '-');
    }
    ti_put(&B, "\x1b\\", 2);
    if (B.bad) goto out;
    *out = (char *)B.p;
    *on = B.n;
    B.p = NULL;
    ok = 1;
out:
    free(B.p);
    free(hist);
    free(sum);
    free(map);
    free(idx);
    free(bits);
    return ok;
}

/* ---- deflate / zlib --------------------------------------------------------- */

typedef struct {
    TiBuf *b;
    uint64_t acc;
    int nb;
} TiBits;

static void ti_bits(TiBits *w, uint32_t v, int n) {
    w->acc |= (uint64_t)v << w->nb;
    w->nb += n;
    if (w->nb >= 32) {
        /* Four bytes at a time; room was reserved up front. */
        if (ti_grow(w->b, 4)) {
            uint8_t *o = w->b->p + w->b->n;
            o[0] = (uint8_t)w->acc;
            o[1] = (uint8_t)(w->acc >> 8);
            o[2] = (uint8_t)(w->acc >> 16);
            o[3] = (uint8_t)(w->acc >> 24);
            w->b->n += 4;
        }
        w->acc >>= 32;
        w->nb -= 32;
    }
}

static void ti_bits_flush(TiBits *w) {
    while (w->nb > 0) {
        ti_putc(w->b, (uint8_t)(w->acc & 255));
        w->acc >>= 8;
        w->nb -= 8;
    }
    w->nb = 0;
    w->acc = 0;
}

static uint32_t ti_rev(uint32_t v, int n) {
    uint32_t r = 0;
    int i;
    for (i = 0; i < n; i++) {
        r = (r << 1) | (v & 1);
        v >>= 1;
    }
    return r;
}

/* The fixed Huffman codes, bit-reversed for the LSB-first writer. */
static uint16_t g_lcode[288];
static uint8_t g_llen[288];
static uint8_t g_dcode[30];
static volatile int g_codes_ok;

static void ti_codes(void) {
    int v;
    if (g_codes_ok) return;
    for (v = 0; v < 288; v++) {
        if (v < 144) {
            g_lcode[v] = (uint16_t)ti_rev((uint32_t)(0x30 + v), 8);
            g_llen[v] = 8;
        } else if (v < 256) {
            g_lcode[v] = (uint16_t)ti_rev((uint32_t)(0x190 + v - 144), 9);
            g_llen[v] = 9;
        } else if (v < 280) {
            g_lcode[v] = (uint16_t)ti_rev((uint32_t)(v - 256), 7);
            g_llen[v] = 7;
        } else {
            g_lcode[v] = (uint16_t)ti_rev((uint32_t)(0xC0 + v - 280), 8);
            g_llen[v] = 8;
        }
    }
    for (v = 0; v < 30; v++) g_dcode[v] = (uint8_t)ti_rev((uint32_t)v, 5);
    g_codes_ok = 1;
}

static inline void ti_lit(TiBits *w, int v) {
    ti_bits(w, g_lcode[v], g_llen[v]);
}

static const uint16_t g_lbase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                     31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t g_lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                   2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t g_dbase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                                     33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                                     1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t g_dext[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                   6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static void ti_match(TiBits *w, int len, int dist) {
    int i;
    for (i = 28; i > 0 && g_lbase[i] > len; i--) {
    }
    ti_lit(w, 257 + i);
    if (g_lext[i]) ti_bits(w, (uint32_t)(len - g_lbase[i]), g_lext[i]);
    for (i = 29; i > 0 && g_dbase[i] > dist; i--) {
    }
    ti_bits(w, g_dcode[i], 5);
    if (g_dext[i]) ti_bits(w, (uint32_t)(dist - g_dbase[i]), g_dext[i]);
}

#define TI_WIN 32768
#define TI_HBITS 15
#ifndef TI_CHAIN
#define TI_CHAIN 8
#endif

int rtx_timg_zlib(const uint8_t *b, size_t n, uint8_t **out, size_t *on) {
    TiBuf B = {0};
    TiBits W;
    int32_t *head = NULL, *prev = NULL;
    size_t i = 0;
    uint32_t a1 = 1, a2 = 0;
    if (out) *out = NULL;
    if (on) *on = 0;
    if (!out || !on || (!b && n)) return 0;
    head = (int32_t *)malloc(sizeof(int32_t) << TI_HBITS);
    prev = (int32_t *)malloc(sizeof(int32_t) * TI_WIN);
    if (!head || !prev) {
        free(head);
        free(prev);
        return 0;
    }
    memset(head, 0xff, sizeof(int32_t) << TI_HBITS);
    ti_codes();
    /* Fixed codes are at most 9 bits a byte (plus the headers). */
    if (!ti_grow(&B, n + n / 7 + 64)) {
        free(head);
        free(prev);
        return 0;
    }
    W.b = &B;
    W.acc = 0;
    W.nb = 0;
    ti_putc(&B, 0x78);
    ti_putc(&B, 0x01);
    ti_bits(&W, 1, 1); /* BFINAL */
    ti_bits(&W, 1, 2); /* fixed Huffman */
    while (i < n) {
        int best = 0, bdist = 0;
        if (i + 3 <= n) {
            uint32_t hsh = ((uint32_t)b[i] << 10 ^ (uint32_t)b[i + 1] << 5 ^ b[i + 2]) &
                           ((1u << TI_HBITS) - 1);
            int32_t cand = head[hsh];
            int chain = TI_CHAIN;
            size_t maxl = n - i < 258 ? n - i : 258;
            while (cand >= 0 && chain-- > 0 && i - (size_t)cand <= TI_WIN - 1) {
                const uint8_t *p = b + cand, *q = b + i;
                size_t l = 0;
                while (l < maxl && p[l] == q[l]) l++;
                if ((int)l > best) {
                    best = (int)l;
                    bdist = (int)(i - (size_t)cand);
                    if (l == maxl) break;
                }
                {
                    int32_t nx = prev[cand % TI_WIN];
                    if (nx >= cand) break;
                    cand = nx;
                }
            }
            prev[i % TI_WIN] = head[hsh];
            head[hsh] = (int32_t)i;
        }
        if (best >= 3) {
            size_t k, end = i + (size_t)best;
            ti_match(&W, best, bdist);
            for (k = i + 1; k < end && k + 3 <= n; k++) {
                uint32_t hsh = ((uint32_t)b[k] << 10 ^ (uint32_t)b[k + 1] << 5 ^ b[k + 2]) &
                               ((1u << TI_HBITS) - 1);
                prev[k % TI_WIN] = head[hsh];
                head[hsh] = (int32_t)k;
            }
            i = end;
        } else {
            ti_lit(&W, b[i]);
            i++;
        }
    }
    ti_lit(&W, 256);
    ti_bits_flush(&W);
    for (i = 0; i < n; i++) {
        a1 = (a1 + b[i]) % 65521u;
        a2 = (a2 + a1) % 65521u;
    }
    ti_putc(&B, (uint8_t)(a2 >> 8));
    ti_putc(&B, (uint8_t)a2);
    ti_putc(&B, (uint8_t)(a1 >> 8));
    ti_putc(&B, (uint8_t)a1);
    free(head);
    free(prev);
    if (B.bad) {
        free(B.p);
        return 0;
    }
    *out = B.p;
    *on = B.n;
    return 1;
}

/* ---- PNG ------------------------------------------------------------------ */

static uint32_t g_crc[256];
static volatile int g_crc_ok;

static uint32_t ti_crc(const uint8_t *p, size_t n, uint32_t c) {
    size_t i;
    if (!g_crc_ok) {
        uint32_t k, v;
        int j;
        for (k = 0; k < 256; k++) {
            v = k;
            for (j = 0; j < 8; j++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            g_crc[k] = v;
        }
        g_crc_ok = 1;
    }
    c = ~c;
    for (i = 0; i < n; i++) c = g_crc[(c ^ p[i]) & 255] ^ (c >> 8);
    return ~c;
}

static void ti_be32(TiBuf *b, uint32_t v) {
    uint8_t t[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    ti_put(b, t, 4);
}

static void ti_chunk(TiBuf *b, const char *type, const uint8_t *d, size_t n) {
    uint32_t c;
    ti_be32(b, (uint32_t)n);
    ti_put(b, type, 4);
    if (n) ti_put(b, d, n);
    c = ti_crc((const uint8_t *)type, 4, 0);
    c = ti_crc(d, n, c);
    ti_be32(b, c);
}

static inline int ti_paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

int rtx_timg_png(const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t stride,
                 uint8_t **out, size_t *on) {
    size_t rowb = (size_t)w * 4 + 1, y;
    uint8_t *raw, *z = NULL, ihdr[13];
    size_t zn = 0;
    TiBuf B = {0};
    if (out) *out = NULL;
    if (on) *on = 0;
    if (!rgba || !w || !h || !out || !on) return 0;
    raw = (uint8_t *)malloc(rowb * h);
    if (!raw) return 0;
    for (y = 0; y < h; y++) {
        const uint8_t *s = rgba + y * stride, *up = y ? rgba + (y - 1) * stride : NULL;
        uint8_t *d = raw + y * rowb;
        size_t x;
        long best = -1;
        int f, bf = 0;
        /* The filter with the least sum of |bytes| (the usual heuristic). */
        for (f = 0; f < 5; f++) {
            long sum = 0;
            if (f >= 2 && !up) continue;
            for (x = 0; x < (size_t)w * 4; x++) {
                int a = x >= 4 ? s[x - 4] : 0, b = up ? up[x] : 0,
                    c = (x >= 4 && up) ? up[x - 4] : 0, v;
                v = f == 0 ? s[x] : f == 1 ? s[x] - a : f == 2 ? s[x] - b
                  : f == 3 ? s[x] - ((a + b) >> 1) : s[x] - ti_paeth(a, b, c);
                v = (int8_t)(uint8_t)v;
                sum += v < 0 ? -v : v;
            }
            if (best < 0 || sum < best) {
                best = sum;
                bf = f;
            }
        }
        d[0] = (uint8_t)bf;
        for (x = 0; x < (size_t)w * 4; x++) {
            int a = x >= 4 ? s[x - 4] : 0, b = up ? up[x] : 0, c = (x >= 4 && up) ? up[x - 4] : 0;
            int v = bf == 0 ? s[x] : bf == 1 ? s[x] - a : bf == 2 ? s[x] - b
                  : bf == 3 ? s[x] - ((a + b) >> 1) : s[x] - ti_paeth(a, b, c);
            d[1 + x] = (uint8_t)v;
        }
    }
    if (!rtx_timg_zlib(raw, rowb * h, &z, &zn)) {
        free(raw);
        return 0;
    }
    free(raw);
    ti_put(&B, "\x89PNG\r\n\x1a\n", 8);
    ihdr[0] = (uint8_t)(w >> 24);
    ihdr[1] = (uint8_t)(w >> 16);
    ihdr[2] = (uint8_t)(w >> 8);
    ihdr[3] = (uint8_t)w;
    ihdr[4] = (uint8_t)(h >> 24);
    ihdr[5] = (uint8_t)(h >> 16);
    ihdr[6] = (uint8_t)(h >> 8);
    ihdr[7] = (uint8_t)h;
    ihdr[8] = 8;
    ihdr[9] = 6;
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    ti_chunk(&B, "IHDR", ihdr, 13);
    ti_chunk(&B, "IDAT", z, zn);
    ti_chunk(&B, "IEND", NULL, 0);
    free(z);
    if (B.bad) {
        free(B.p);
        return 0;
    }
    *out = B.p;
    *on = B.n;
    return 1;
}

/* ---- base64 --------------------------------------------------------------- */

size_t rtx_timg_b64_len(size_t n) {
    return 4 * ((n + 2) / 3);
}

size_t rtx_timg_b64(const uint8_t *b, size_t n, char *out) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, o = 0;
    for (i = 0; i + 2 < n; i += 3) {
        uint32_t v = (uint32_t)b[i] << 16 | (uint32_t)b[i + 1] << 8 | b[i + 2];
        out[o++] = T[v >> 18];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = T[(v >> 6) & 63];
        out[o++] = T[v & 63];
    }
    if (i < n) {
        uint32_t v = (uint32_t)b[i] << 16 | (i + 1 < n ? (uint32_t)b[i + 1] << 8 : 0);
        out[o++] = T[v >> 18];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = i + 1 < n ? T[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = 0;
    return o;
}

/* ---- kitty Unicode placeholders ----------------------------------------------- */

/* kitty's rowcolumn-diacritics.txt (combining class 230, Unicode 6.0). */
static const uint32_t g_diac[RTX_TIMG_PH_MAX] = {
0x0305, 0x030D, 0x030E, 0x0310, 0x0312, 0x033D, 0x033E, 0x033F, 0x0346, 0x034A, 0x034B, 0x034C,
0x0350, 0x0351, 0x0352, 0x0357, 0x035B, 0x0363, 0x0364, 0x0365, 0x0366, 0x0367, 0x0368, 0x0369,
0x036A, 0x036B, 0x036C, 0x036D, 0x036E, 0x036F, 0x0483, 0x0484, 0x0485, 0x0486, 0x0487, 0x0592,
0x0593, 0x0594, 0x0595, 0x0597, 0x0598, 0x0599, 0x059C, 0x059D, 0x059E, 0x059F, 0x05A0, 0x05A1,
0x05A8, 0x05A9, 0x05AB, 0x05AC, 0x05AF, 0x05C4, 0x0610, 0x0611, 0x0612, 0x0613, 0x0614, 0x0615,
0x0616, 0x0617, 0x0657, 0x0658, 0x0659, 0x065A, 0x065B, 0x065D, 0x065E, 0x06D6, 0x06D7, 0x06D8,
0x06D9, 0x06DA, 0x06DB, 0x06DC, 0x06DF, 0x06E0, 0x06E1, 0x06E2, 0x06E4, 0x06E7, 0x06E8, 0x06EB,
0x06EC, 0x0730, 0x0732, 0x0733, 0x0735, 0x0736, 0x073A, 0x073D, 0x073F, 0x0740, 0x0741, 0x0743,
0x0745, 0x0747, 0x0749, 0x074A, 0x07EB, 0x07EC, 0x07ED, 0x07EE, 0x07EF, 0x07F0, 0x07F1, 0x07F3,
0x0816, 0x0817, 0x0818, 0x0819, 0x081B, 0x081C, 0x081D, 0x081E, 0x081F, 0x0820, 0x0821, 0x0822,
0x0823, 0x0825, 0x0826, 0x0827, 0x0829, 0x082A, 0x082B, 0x082C, 0x082D, 0x0951, 0x0953, 0x0954,
0x0F82, 0x0F83, 0x0F86, 0x0F87, 0x135D, 0x135E, 0x135F, 0x17DD, 0x193A, 0x1A17, 0x1A75, 0x1A76,
0x1A77, 0x1A78, 0x1A79, 0x1A7A, 0x1A7B, 0x1A7C, 0x1B6B, 0x1B6D, 0x1B6E, 0x1B6F, 0x1B70, 0x1B71,
0x1B72, 0x1B73, 0x1CD0, 0x1CD1, 0x1CD2, 0x1CDA, 0x1CDB, 0x1CE0, 0x1DC0, 0x1DC1, 0x1DC3, 0x1DC4,
0x1DC5, 0x1DC6, 0x1DC7, 0x1DC8, 0x1DC9, 0x1DCB, 0x1DCC, 0x1DD1, 0x1DD2, 0x1DD3, 0x1DD4, 0x1DD5,
0x1DD6, 0x1DD7, 0x1DD8, 0x1DD9, 0x1DDA, 0x1DDB, 0x1DDC, 0x1DDD, 0x1DDE, 0x1DDF, 0x1DE0, 0x1DE1,
0x1DE2, 0x1DE3, 0x1DE4, 0x1DE5, 0x1DE6, 0x1DFE, 0x20D0, 0x20D1, 0x20D4, 0x20D5, 0x20D6, 0x20D7,
0x20DB, 0x20DC, 0x20E1, 0x20E7, 0x20E9, 0x20F0, 0x2CEF, 0x2CF0, 0x2CF1, 0x2DE0, 0x2DE1, 0x2DE2,
0x2DE3, 0x2DE4, 0x2DE5, 0x2DE6, 0x2DE7, 0x2DE8, 0x2DE9, 0x2DEA, 0x2DEB, 0x2DEC, 0x2DED, 0x2DEE,
0x2DEF, 0x2DF0, 0x2DF1, 0x2DF2, 0x2DF3, 0x2DF4, 0x2DF5, 0x2DF6, 0x2DF7, 0x2DF8, 0x2DF9, 0x2DFA,
0x2DFB, 0x2DFC, 0x2DFD, 0x2DFE, 0x2DFF, 0xA66F, 0xA67C, 0xA67D, 0xA6F0, 0xA6F1, 0xA8E0, 0xA8E1,
0xA8E2, 0xA8E3, 0xA8E4, 0xA8E5, 0xA8E6, 0xA8E7, 0xA8E8, 0xA8E9, 0xA8EA, 0xA8EB, 0xA8EC, 0xA8ED,
0xA8EE, 0xA8EF, 0xA8F0, 0xA8F1, 0xAAB0, 0xAAB2, 0xAAB3, 0xAAB7, 0xAAB8, 0xAABE, 0xAABF, 0xAAC1,
0xFE20, 0xFE21, 0xFE22, 0xFE23, 0xFE24, 0xFE25, 0xFE26, 0x10A0F, 0x10A38, 0x1D185, 0x1D186, 0x1D187,
0x1D188, 0x1D189, 0x1D1AA, 0x1D1AB, 0x1D1AC, 0x1D1AD, 0x1D242, 0x1D243, 0x1D244};

int rtx_timg_kitty_ph(int row, int col, int msb, char *out) {
    int n;
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (row >= RTX_TIMG_PH_MAX) row = RTX_TIMG_PH_MAX - 1;
    if (col >= RTX_TIMG_PH_MAX) col = RTX_TIMG_PH_MAX - 1;
    n = rtx_timg_utf8(0x10EEEE, out);
    n += rtx_timg_utf8(g_diac[row], out + n);
    n += rtx_timg_utf8(g_diac[col], out + n);
    if (msb > 0 && msb < RTX_TIMG_PH_MAX) n += rtx_timg_utf8(g_diac[msb], out + n);
    return n;
}

int rtx_timg_kitty_diac(uint32_t cp) {
    int i;
    for (i = 0; i < RTX_TIMG_PH_MAX; i++)
        if (g_diac[i] == cp) return i;
    return -1;
}
