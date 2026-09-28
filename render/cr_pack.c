/*
 * cctext-render asset pack: build (stb's zlib, vendored inside plutovg)
 * and load (Wuffs' zlib decoder, third_party/wuffs). cr_pack.h has the
 * layout.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WUFFS_IMPLEMENTATION
#define WUFFS_CONFIG__STATIC_FUNCTIONS
#define WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULE__BASE
#define WUFFS_CONFIG__MODULE__ADLER32
#define WUFFS_CONFIG__MODULE__CRC32
#define WUFFS_CONFIG__MODULE__DEFLATE
#define WUFFS_CONFIG__MODULE__ZLIB
#include "wuffs-v0.4.c"

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "plutovg-stb-image-write.h"

#include "cr_pack.h"
#include "cr_sha256.h"

#define CR_PACK_MAGIC "CRPK0003"
#define CR_PACK_MAX_ENTRIES 4096u
#define CR_PACK_MAX_RAW (256u << 20)
#define CR_PACK_BUILD_MAX 512 /* manifest entries one build takes */

static uint64_t cr_fnv(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

uint8_t *cr_zlib_compress(const uint8_t *in, size_t n, size_t *out_n)
{
    int ol = 0;
    uint8_t *o;
    if (n > (size_t)0x7fffffff) return NULL;
    o = stbi_zlib_compress((unsigned char *)in, (int)n, &ol, 24);
    *out_n = (size_t)ol;
    return o; /* STBIW_MALLOC is malloc */
}

int cr_zlib_decompress(const uint8_t *in, size_t n, uint8_t *out, size_t raw_len)
{
    wuffs_zlib__decoder *dec = wuffs_zlib__decoder__alloc();
    wuffs_base__range_ii_u64 wl;
    uint8_t *work = NULL;
    int rc = -1;
    if (!dec) return -1;
    wl = wuffs_zlib__decoder__workbuf_len(dec);
    if (wl.max_incl) {
        work = malloc((size_t)wl.max_incl);
        if (!work) {
            free(dec);
            return -1;
        }
    }
    {
        wuffs_base__io_buffer dst = wuffs_base__ptr_u8__writer(out, raw_len);
        wuffs_base__io_buffer src = wuffs_base__ptr_u8__reader((uint8_t *)in, n, true);
        wuffs_base__status st = wuffs_zlib__decoder__transform_io(
            dec, &dst, &src, wuffs_base__make_slice_u8(work, (size_t)wl.max_incl));
        /* One call: the whole stream is in memory and the output buffer is
         * exactly raw_len, so any suspension means a corrupt entry. */
        if (wuffs_base__status__is_ok(&st) && dst.meta.wi == raw_len) rc = 0;
    }
    free(work);
    free(dec);
    return rc;
}

/* ---- reader --------------------------------------------------------- */

int cr_pack_open(CrPack *p, const uint8_t *buf, size_t n)
{
    uint32_t cnt;
    size_t o = 12, end_payload = 0;
    uint64_t want;
    memset(p, 0, sizeof *p);
    if (n < 20 || memcmp(buf, CR_PACK_MAGIC, 8) != 0) return -1;
    memcpy(&want, buf + n - 8, 8);
    n -= 8;
    memcpy(&cnt, buf + 8, 4);
    if (cnt > CR_PACK_MAX_ENTRIES) return -1;
    p->e = calloc(cnt ? cnt : 1, sizeof *p->e);
    if (!p->e) return -1;
    /* The table: every entry's name, lengths and offset, together at the
     * front (one or two pages). */
    for (uint32_t i = 0; i < cnt; i++) {
        uint16_t nl;
        CrAsset *a = &p->e[i];
        if (o + 2 > n) return -1;
        memcpy(&nl, buf + o, 2);
        o += 2;
        if (o + nl + 12 > n) return -1;
        a->name = strndup((const char *)buf + o, nl);
        if (!a->name) return -1;
        o += nl;
        memcpy(&a->raw_len, buf + o, 4);
        memcpy(&a->comp_len, buf + o + 4, 4);
        memcpy(&a->off, buf + o + 8, 4);
        o += 12;
        if (a->raw_len > CR_PACK_MAX_RAW) return -1;
        p->n++;
    }
    if (cr_fnv(buf, o) != want) return -1;
    /* Payloads: after the table, in order, inside the file. */
    end_payload = o;
    for (int i = 0; i < p->n; i++) {
        CrAsset *a = &p->e[i];
        if (a->off != end_payload || a->comp_len > n - a->off) return -1;
        a->comp = buf + a->off;
        end_payload = (size_t)a->off + a->comp_len;
    }
    return end_payload == n ? 0 : -1;
}

CrAsset *cr_pack_find(CrPack *p, const char *name)
{
    for (int i = 0; i < p->n; i++)
        if (strcmp(p->e[i].name, name) == 0) return &p->e[i];
    return NULL;
}

const uint8_t *cr_asset_data(CrAsset *a)
{
    uint8_t *o;
    if (a->raw) return a->raw;
    o = malloc((size_t)a->raw_len + 1);
    if (!o) return NULL;
    if (cr_zlib_decompress(a->comp, a->comp_len, o, a->raw_len) != 0) {
        free(o);
        return NULL;
    }
    o[a->raw_len] = 0;
    a->raw = o;
    return o;
}

void cr_asset_drop(CrAsset *a) {
    if (!a) return;
    free(a->raw);
    a->raw = NULL;
}

/* ---- build step ------------------------------------------------------ */

static uint8_t *cr_slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    long len;
    uint8_t *b;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0 ||
        (unsigned long)len > CR_PACK_MAX_RAW) {
        fclose(f);
        return NULL;
    }
    b = malloc((size_t)len + 1);
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) {
        free(b);
        b = NULL;
    }
    fclose(f);
    if (b) *n = (size_t)len;
    return b;
}

typedef struct {
    char *name;
    uint8_t *raw;
    size_t raw_len;
    uint8_t *comp; /* zlib of raw, made before the table is written */
    size_t comp_len;
} CrPackIn;

static void cr_lower(char *s)
{
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
}

/* A growing text buffer for bundles. */
typedef struct {
    char *b;
    size_t n, cap;
} CrSb;

static int cr_sb_add(CrSb *s, const void *p, size_t n)
{
    if (s->n + n + 1 > s->cap) {
        size_t c = (s->n + n + 1) * 2;
        char *nb = realloc(s->b, c);
        if (!nb) return -1;
        s->b = nb;
        s->cap = c;
    }
    memcpy(s->b + s->n, p, n);
    s->n += n;
    s->b[s->n] = 0;
    return 0;
}

static int cr_is_hex64(const char *h)
{
    size_t i;
    for (i = 0; i < 64; i++)
        if (!((h[i] >= '0' && h[i] <= '9') || (h[i] >= 'a' && h[i] <= 'f'))) return 0;
    return h[64] == 0;
}

/* A manifest input read whole; with `hash` ("sha256=<hex>") refused
 * unless its SHA-256 matches (a vendored file that is not the pinned one). */
static uint8_t *cr_slurp_pinned(const char *manifest, const char *path, const char *hash,
                                size_t *n) {
    uint8_t *d = cr_slurp(path, n);
    char got[65];
    if (!d) {
        fprintf(stderr, "build-pack: cannot read %s\n", path);
        return NULL;
    }
    if (!hash) return d;
    if (strncmp(hash, "sha256=", 7) != 0 || !cr_is_hex64(hash + 7)) {
        fprintf(stderr, "build-pack: %s: bad hash field %s\n", manifest, hash);
        free(d);
        return NULL;
    }
    cr_sha256_hex(d, *n, got);
    if (strcmp(got, hash + 7) != 0) {
        fprintf(stderr,
                "build-pack: %s: SHA-256 %s, the manifest records %s: refusing a file that is "
                "not the vendored one\n",
                path, got, hash + 7);
        free(d);
        return NULL;
    }
    return d;
}

/* The hash header: rewritten only when its text changes (the helper's
 * main object depends on its mtime). */
static int cr_write_hash_header(const char *path, const char *rows)
{
    char tmp[4200];
    size_t n;
    FILE *f;
    CrSb t = {0};
    static const char head[] =
        "/* Generated by cctext-render --build-pack (render/cr_pack.c): the SHA-256\n"
        " * of each JavaScript entry's stored bytes in the pack built with it. The\n"
        " * helper refuses bytecode that does not match (docs/images.md). */\n"
        "#define CR_PACK_JS_HASHES \\\n";
    if (cr_sb_add(&t, head, sizeof head - 1) != 0) return -1;
    {
        /* rows are "    {..},\n"; each line but the last needs a continuation */
        const char *r = rows;
        while (*r) {
            const char *e = strchr(r, '\n');
            size_t l = e ? (size_t)(e - r) : strlen(r);
            if (cr_sb_add(&t, r, l) != 0 || cr_sb_add(&t, " \\\n", 3) != 0) return -1;
            r += l + (e ? 1 : 0);
        }
        if (cr_sb_add(&t, "    {0, 0}\n", 11) != 0) return -1;
    }
    {
        size_t on = 0;
        uint8_t *old = cr_slurp(path, &on);
        int same = old && on == t.n && memcmp(old, t.b, on) == 0;
        free(old);
        if (same) {
            free(t.b);
            return 0;
        }
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "wb");
    n = f ? fwrite(t.b, 1, t.n, f) : 0;
    if (!f || n != t.n || fclose(f) != 0 || rename(tmp, path) != 0) {
        perror(path);
        free(t.b);
        return -1;
    }
    free(t.b);
    return 0;
}

/* Manifest lines ('|' separated; '#' comments; paths relative to the
 * working directory, the repository root at build time):
 *   font|<family>|<bold 0/1>|<italic 0/1>|<path.ttf>
 *   bundle|<name>                  start a JavaScript bundle (entry js:<name>)
 *   raw|<path>[|sha256=<hex>]      append a file verbatim (and a ";\n"),
 *                                  refusing it unless its SHA-256 matches
 *   wrap|<key>|<path>[|sha256=<hex>]
 *                                  append the file verbatim as the body of
 *                                  __cctextMods["<key>"] = function () {...};
 *                                  (a module the bundle's own loader runs on
 *                                  demand: MathJax's components)
 *   end                            compile the bundle to QuickJS bytecode
 *   src|<key>|<path>[|sha256=<hex>]
 *                                  (outside a bundle) the file's bytes as
 *                                  entry src:<key>: JavaScript source the
 *                                  helper evaluates only when a render asks
 *                                  for it (MathJax's TeX extensions and
 *                                  dynamic font files); never bytecode
 * `compile` turns a bundle's source into bytecode (cr_js_compile); NULL
 * refuses bundles. `hash_hdr` (may be NULL) receives a C header with the
 * SHA-256 of every js: and src: entry's stored bytes (cr_pack.h). */
int cr_pack_build(const char *manifest, const char *out_path, CrPackCompile compile,
                  const char *hash_hdr)
{
    FILE *m = fopen(manifest, "r"), *o;
    CrPackIn in[CR_PACK_BUILD_MAX];
    int nin = 0, rc = 1;
    char line[4096], tmp[4096], bundle[128];
    size_t total_raw = 0, total_comp = 0;
    uint8_t *img = NULL;
    size_t img_n = 0, img_cap = 0;
    uint64_t th = 0;
    size_t off = 0;
    CrSb src = {0}, hdr = {0};
    int in_bundle = 0;
    if (!m) {
        perror(manifest);
        return 1;
    }
    bundle[0] = 0;
    memset(in, 0, sizeof in);
    while (fgets(line, sizeof line, m)) {
        char *f[6] = {0}, *p = line, *t;
        int nf = 0;
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#') continue;
        while (nf < 6 && (t = strsep(&p, "|"))) f[nf++] = t;
        if (nin >= CR_PACK_BUILD_MAX) {
            fprintf(stderr, "build-pack: too many entries\n");
            goto out;
        }
        if (strcmp(f[0], "bundle") == 0 && nf == 2 && !in_bundle) {
            snprintf(bundle, sizeof bundle, "%s", f[1]);
            src.n = 0;
            in_bundle = 1;
        } else if (strcmp(f[0], "raw") == 0 && (nf == 2 || nf == 3) && in_bundle) {
            size_t n = 0;
            uint8_t *d = cr_slurp_pinned(manifest, f[1], nf == 3 ? f[2] : NULL, &n);
            if (!d) goto out;
            if (cr_sb_add(&src, d, n) != 0 || cr_sb_add(&src, "\n;\n", 3) != 0) {
                free(d);
                goto out;
            }
            free(d);
        } else if (strcmp(f[0], "wrap") == 0 && (nf == 3 || nf == 4) && in_bundle) {
            size_t n = 0;
            char head[640];
            int hn;
            uint8_t *d;
            if (strpbrk(f[1], "\"\\") || !f[1][0]) {
                fprintf(stderr, "build-pack: %s: bad module key %s\n", manifest, f[1]);
                goto out;
            }
            d = cr_slurp_pinned(manifest, f[2], nf == 4 ? f[3] : NULL, &n);
            if (!d) goto out;
            hn = snprintf(head, sizeof head, "\n;__cctextMods[\"%s\"] = function () {\n", f[1]);
            if (hn <= 0 || (size_t)hn >= sizeof head || cr_sb_add(&src, head, (size_t)hn) != 0 ||
                cr_sb_add(&src, d, n) != 0 || cr_sb_add(&src, "\n};\n", 4) != 0) {
                free(d);
                goto out;
            }
            free(d);
        } else if (strcmp(f[0], "src") == 0 && (nf == 3 || nf == 4) && !in_bundle) {
            char nm[512];
            size_t n = 0;
            uint8_t *d = cr_slurp_pinned(manifest, f[2], nf == 4 ? f[3] : NULL, &n);
            if (!d) goto out;
            snprintf(nm, sizeof nm, "src:%s", f[1]);
            in[nin].name = strdup(nm);
            in[nin].raw = d;
            in[nin].raw_len = n;
            nin++;
        } else if (strcmp(f[0], "end") == 0 && nf == 1 && in_bundle) {
            char nm[160], err[512];
            uint8_t *bc = NULL;
            size_t bn = 0;
            in_bundle = 0;
            if (!compile) {
                fprintf(stderr, "build-pack: this build cannot compile JavaScript\n");
                goto out;
            }
            if (compile(bundle, src.b ? src.b : "", src.n, &bc, &bn, err, sizeof err) != 0) {
                fprintf(stderr, "build-pack: js:%s: %s\n", bundle, err);
                goto out;
            }
            fprintf(stderr, "build-pack: js:%s: %zu bytes of source -> %zu bytes of bytecode\n",
                    bundle, src.n, bn);
            snprintf(nm, sizeof nm, "js:%s", bundle);
            in[nin].name = strdup(nm);
            in[nin].raw = bc;
            in[nin].raw_len = bn;
            nin++;
        } else if (strcmp(f[0], "font") == 0 && nf == 5 && !in_bundle) {
            char nm[512];
            size_t n = 0;
            uint8_t *d;
            snprintf(nm, sizeof nm, "font:%s:%d:%d", f[1], atoi(f[2]) != 0, atoi(f[3]) != 0);
            cr_lower(nm);
            d = cr_slurp(f[4], &n);
            if (!d) {
                fprintf(stderr, "build-pack: cannot read %s\n", f[4]);
                goto out;
            }
            in[nin].name = strdup(nm);
            in[nin].raw = d;
            in[nin].raw_len = n;
            nin++;
        } else {
            fprintf(stderr, "build-pack: %s: bad line: %s\n", manifest, line);
            goto out;
        }
    }
    if (in_bundle) {
        fprintf(stderr, "build-pack: %s: bundle %s has no end\n", manifest, bundle);
        goto out;
    }
    /* Assemble in memory (layout: cr_pack.h). */
#define CR_PUT(p_, n_)                                                              \
    do {                                                                            \
        size_t n__ = (n_);                                                          \
        if (img_n + n__ > img_cap) {                                                \
            size_t c__ = (img_n + n__) * 2;                                         \
            uint8_t *b__ = realloc(img, c__);                                       \
            if (!b__) goto out;                                                     \
            img = b__;                                                              \
            img_cap = c__;                                                          \
        }                                                                           \
        memcpy(img + img_n, (p_), n__);                                             \
        img_n += n__;                                                               \
    } while (0)
    /* Compress (and round-trip) every entry first: the table at the front
     * holds each payload's offset. */
    for (int i = 0; i < nin; i++) {
        size_t cn = 0;
        uint8_t *c = cr_zlib_compress(in[i].raw, in[i].raw_len, &cn);
        if (!c) goto out;
        in[i].comp = c;
        in[i].comp_len = cn;
        /* Round-trip through the run-time decoder before shipping it. */
        {
            uint8_t *chk = malloc(in[i].raw_len + 1);
            int bad = !chk || cr_zlib_decompress(c, cn, chk, in[i].raw_len) != 0 ||
                      memcmp(chk, in[i].raw, in[i].raw_len) != 0;
            free(chk);
            if (bad) {
                fprintf(stderr, "build-pack: %s: zlib round trip failed\n", in[i].name);
                goto out;
            }
        }
        if (strncmp(in[i].name, "js:", 3) == 0 || strncmp(in[i].name, "src:", 4) == 0) {
            char hx[65], ln[256];
            int k;
            cr_sha256_hex(c, cn, hx);
            k = snprintf(ln, sizeof ln, "    {\"%s\", \"%s\"},\n", in[i].name, hx);
            if (cr_sb_add(&hdr, ln, (size_t)k) != 0) goto out;
        }
        total_raw += in[i].raw_len;
        total_comp += cn;
    }
    off = 12;
    for (int i = 0; i < nin; i++) off += 2 + strlen(in[i].name) + 12;
    {
        uint32_t cnt = (uint32_t)nin;
        CR_PUT(CR_PACK_MAGIC, 8);
        CR_PUT(&cnt, 4);
    }
    for (int i = 0; i < nin; i++) {
        uint16_t nl = (uint16_t)strlen(in[i].name);
        uint32_t rl = (uint32_t)in[i].raw_len, cl = (uint32_t)in[i].comp_len, ol = (uint32_t)off;
        if (off + in[i].comp_len > 0xffffffffu) {
            fprintf(stderr, "build-pack: pack over 4 GB\n");
            goto out;
        }
        CR_PUT(&nl, 2);
        CR_PUT(in[i].name, nl);
        CR_PUT(&rl, 4);
        CR_PUT(&cl, 4);
        CR_PUT(&ol, 4);
        off += in[i].comp_len;
    }
    th = cr_fnv(img, img_n); /* the trailer: header and table */
    for (int i = 0; i < nin; i++) CR_PUT(in[i].comp, in[i].comp_len);
    CR_PUT(&th, 8);
#undef CR_PUT
    snprintf(tmp, sizeof tmp, "%s.tmp", out_path);
    o = fopen(tmp, "wb");
    if (!o) {
        perror(tmp);
        goto out;
    }
    if (fwrite(img, 1, img_n, o) != img_n || fclose(o) != 0) {
        perror(tmp);
        goto out;
    }
    if (rename(tmp, out_path) != 0) {
        perror(out_path);
        goto out;
    }
    fprintf(stderr, "build-pack: %s: %d entries, %zu -> %zu bytes\n", out_path, nin, total_raw,
            total_comp);
    if (hash_hdr && cr_write_hash_header(hash_hdr, hdr.b ? hdr.b : "") != 0) goto out;
    rc = 0;
out:
    fclose(m);
    free(src.b);
    free(hdr.b);
    for (int i = 0; i < nin; i++) {
        free(in[i].name);
        free(in[i].raw);
        free(in[i].comp);
    }
    free(img);
    return rc;
}
