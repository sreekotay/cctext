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

#define CR_PACK_MAGIC "CRPK0002"
#define CR_PACK_MAX_ENTRIES 4096u
#define CR_PACK_MAX_RAW (256u << 20)

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
    size_t o = 12;
    uint64_t want;
    memset(p, 0, sizeof *p);
    if (n < 20 || memcmp(buf, CR_PACK_MAGIC, 8) != 0) return -1;
    memcpy(&want, buf + n - 8, 8);
    if (cr_fnv(buf, n - 8) != want) return -1;
    n -= 8;
    memcpy(&cnt, buf + 8, 4);
    if (cnt > CR_PACK_MAX_ENTRIES) return -1;
    p->e = calloc(cnt ? cnt : 1, sizeof *p->e);
    if (!p->e) return -1;
    for (uint32_t i = 0; i < cnt; i++) {
        uint16_t nl;
        CrAsset *a = &p->e[i];
        if (o + 2 > n) return -1;
        memcpy(&nl, buf + o, 2);
        o += 2;
        if (o + nl + 8 > n) return -1;
        a->name = strndup((const char *)buf + o, nl);
        if (!a->name) return -1;
        o += nl;
        memcpy(&a->raw_len, buf + o, 4);
        memcpy(&a->comp_len, buf + o + 4, 4);
        o += 8;
        if (a->comp_len > n - o || a->raw_len > CR_PACK_MAX_RAW) return -1;
        a->comp = buf + o;
        o += a->comp_len;
        p->n++;
    }
    return o == n ? 0 : -1;
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
} CrPackIn;

static void cr_lower(char *s)
{
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
}

/* Manifest lines ('|' separated; '#' comments; paths relative to the
 * working directory, the repository root at build time):
 *   font|<family>|<bold 0/1>|<italic 0/1>|<path.ttf>
 * Later steps add `bundle|...` / `end` for QuickJS bytecode. */
int cr_pack_build(const char *manifest, const char *out_path)
{
    FILE *m = fopen(manifest, "r"), *o;
    CrPackIn in[256];
    int nin = 0, rc = 1;
    char line[4096], tmp[4096];
    size_t total_raw = 0, total_comp = 0;
    uint8_t *img = NULL;
    size_t img_n = 0, img_cap = 0;
    if (!m) {
        perror(manifest);
        return 1;
    }
    while (fgets(line, sizeof line, m)) {
        char *f[6] = {0}, *p = line, *t;
        int nf = 0;
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#') continue;
        while (nf < 6 && (t = strsep(&p, "|"))) f[nf++] = t;
        if (strcmp(f[0], "font") == 0 && nf == 5 && nin < 256) {
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
    /* Assemble in memory: the trailer hashes every byte. */
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
    {
        uint32_t cnt = (uint32_t)nin;
        CR_PUT(CR_PACK_MAGIC, 8);
        CR_PUT(&cnt, 4);
    }
    for (int i = 0; i < nin; i++) {
        size_t cn = 0;
        uint8_t *c = cr_zlib_compress(in[i].raw, in[i].raw_len, &cn);
        uint16_t nl = (uint16_t)strlen(in[i].name);
        uint32_t rl = (uint32_t)in[i].raw_len, cl = (uint32_t)cn;
        if (!c) goto out;
        /* Round-trip through the run-time decoder before shipping it. */
        {
            uint8_t *chk = malloc(in[i].raw_len + 1);
            int bad = !chk || cr_zlib_decompress(c, cn, chk, in[i].raw_len) != 0 ||
                      memcmp(chk, in[i].raw, in[i].raw_len) != 0;
            free(chk);
            if (bad) {
                fprintf(stderr, "build-pack: %s: zlib round trip failed\n", in[i].name);
                free(c);
                goto out;
            }
        }
        CR_PUT(&nl, 2);
        CR_PUT(in[i].name, nl);
        CR_PUT(&rl, 4);
        CR_PUT(&cl, 4);
        CR_PUT(c, cn);
        total_raw += in[i].raw_len;
        total_comp += cn;
        free(c);
    }
    {
        uint64_t h = cr_fnv(img, img_n);
        CR_PUT(&h, 8);
    }
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
    rc = 0;
out:
    fclose(m);
    for (int i = 0; i < nin; i++) {
        free(in[i].name);
        free(in[i].raw);
    }
    free(img);
    return rc;
}
