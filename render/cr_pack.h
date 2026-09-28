/*
 * cctext-render asset pack (docs/images.md, "Renderer"). Built by the
 * helper itself at build time (`cctext-render --build-pack`), read before
 * the sandbox is on. Layout, little-endian:
 *
 *   "CRPK0002" | u32 count | count x entry | u64 fnv1a-64 of every byte before it
 *   entry: u16 name_len | name | u32 raw_len | u32 comp_len | zlib[comp_len]
 *
 * Entries are zlib streams (stb's deflate at build time; Wuffs inflates
 * them at run time, checking each stream's Adler-32). Names:
 *   font:<family>:<bold>:<italic>   a TrueType face (family lowercase)
 *   js:<bundle>                     QuickJS bytecode (render/js + third_party)
 *
 * The FNV trailer catches a damaged file. A js: entry is also checked
 * against the SHA-256 of its stored bytes compiled into the helper
 * (CR_PACK_JS_HASHES, a header the build step writes) before QuickJS
 * reads it: bytecode is trusted input to QuickJS, never to be read from
 * anything but the pack this build made.
 */
#ifndef CR_PACK_H
#define CR_PACK_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *name;
    uint32_t raw_len, comp_len;
    const uint8_t *comp;   /* into the pack buffer */
    uint8_t *raw;          /* inflated on first use, kept */
} CrAsset;

typedef struct {
    CrAsset *e;
    int n;
} CrPack;

/* Parse a pack held in buf[0, n) (buf must outlive the pack). 0 = ok. */
int cr_pack_open(CrPack *p, const uint8_t *buf, size_t n);
CrAsset *cr_pack_find(CrPack *p, const char *name);
/* Inflated bytes (raw_len of them), or NULL when the stream is corrupt. */
const uint8_t *cr_asset_data(CrAsset *a);

/* zlib, for the build step (stb) and tests. The result is malloc'd. */
uint8_t *cr_zlib_compress(const uint8_t *in, size_t n, size_t *out_n);
int cr_zlib_decompress(const uint8_t *in, size_t n, uint8_t *out, size_t raw_len);

/* The build step: manifest -> pack (cr_pack.c documents the manifest).
 * `compile` makes bytecode of a bundle (NULL: bundles are refused);
 * `hash_hdr` (NULL: none) receives the CR_PACK_JS_HASHES header, rewritten
 * only when it changes. Returns 0 or prints why and 1. */
typedef int (*CrPackCompile)(const char *name, const char *src, size_t n, uint8_t **out,
                             size_t *out_n, char *err, size_t errcap);
int cr_pack_build(const char *manifest, const char *out_path, CrPackCompile compile,
                  const char *hash_hdr);

#endif /* CR_PACK_H */
