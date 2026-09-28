/*
 * cctext-render asset pack (docs/images.md, "Renderer"). Built by the
 * helper itself at build time (`cctext-render --build-pack`), read before
 * the sandbox is on. Layout, little-endian:
 *
 *   "CRPK0003" | u32 count | table | payloads | u64 trailer
 *   table:    count x {u16 name_len | name | u32 raw_len | u32 comp_len
 *                      | u32 off}   (off: the payload's file offset)
 *   payloads: each entry's zlib[comp_len], in table order, back to back
 *   trailer:  fnv1a-64 of the 12-byte header and the table
 *
 * A helper maps the pack read-only and touches only what it uses: the
 * table, the fonts, and the bundle its slot runs (an SVG helper never
 * pages in the math JavaScript). The table sits together at the front so
 * that reading it faults in a page or two, not a page next to every
 * payload. The trailer covers the table, which every open reads; a
 * payload is checked when it is used: Wuffs verifies each zlib stream's
 * Adler-32 as it inflates it, and a js: / src: entry's SHA-256 is checked
 * before that (below).
 *
 * Entries are zlib streams (miniz's deflate, level 9, at build time;
 * Wuffs inflates them at run time, checking each stream's Adler-32). Names:
 *   font:<family>:<bold>:<italic>   a TrueType face (family lowercase)
 *   js:<bundle>                     QuickJS bytecode (render/js + third_party)
 *   src:<key>                       JavaScript source a bundle loads on
 *                                   demand (math: MathJax's TeX extensions
 *                                   and dynamic font files)
 *
 * The FNV trailer catches a damaged table. A js: or src: entry is also
 * checked against the SHA-256 of its stored bytes compiled into the helper
 * (CR_PACK_JS_HASHES, a header the build step writes) before QuickJS
 * reads it: bytecode is trusted input to QuickJS, never to be read from
 * anything but the pack this build made (and source is held to the same
 * rule, so everything the engines run is what this build pinned).
 */
#ifndef CR_PACK_H
#define CR_PACK_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *name;
    uint32_t raw_len, comp_len;
    uint32_t off;          /* payload offset in the pack */
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
/* Free the inflated copy (the next cr_asset_data inflates again). */
void cr_asset_drop(CrAsset *a);

/* zlib, for the build step (miniz) and tests. The result is malloc'd. */
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
