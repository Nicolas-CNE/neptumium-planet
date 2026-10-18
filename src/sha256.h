/*
 * sha256.h — Neptumium Planet: self-contained SHA-256.
 * ====================================================
 *
 * Vendored (tiny, public-domain-style) so Planet has no OpenSSL dependency:
 * package integrity is the one thing a package manager must not outsource to
 * an optional library. Used to verify downloaded .pkg.tar.zst payloads against
 * the repo database's %SHA256SUM% before extraction.
 */
#ifndef NEPTUMIUM_SHA256_H
#define NEPTUMIUM_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_SIZE 32

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buf[64];
    size_t   buflen;
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t len);
void sha256_final(sha256_ctx *c, uint8_t out[SHA256_DIGEST_SIZE]);

/* One-shot: hash `len` bytes into `out`. */
void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST_SIZE]);

/* Lowercase hex encoding of a digest (64 chars + NUL). */
void sha256_hex(const uint8_t digest[SHA256_DIGEST_SIZE], char out[65]);

#endif /* NEPTUMIUM_SHA256_H */
