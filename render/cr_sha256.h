/*
 * SHA-256 (FIPS 180-4), small and portable. cctext-render uses it at build
 * time (the vendored mermaid.min.js must match its recorded hash) and at
 * run time (the pack's JavaScript bytecode must match the hash compiled
 * into the helper before QuickJS reads it; docs/images.md, "Mermaid").
 */
#ifndef CR_SHA256_H
#define CR_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t nbuf;
} CrSha256;

void cr_sha256_init(CrSha256 *s);
void cr_sha256_update(CrSha256 *s, const void *p, size_t n);
void cr_sha256_final(CrSha256 *s, uint8_t out[32]);
/* One shot; hex is 64 lowercase digits and a NUL. */
void cr_sha256(const void *p, size_t n, uint8_t out[32]);
void cr_sha256_hex(const void *p, size_t n, char hex[65]);

#endif /* CR_SHA256_H */
