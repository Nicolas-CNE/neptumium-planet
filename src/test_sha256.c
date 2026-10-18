/*
 * test_sha256.c — verify the vendored SHA-256 against the standard vectors.
 * ===========================================================================
 *
 * Planet refuses to unpack any package whose payload does not match the digest
 * published in the repository database, so a single wrong bit in this file
 * would either reject every package or (far worse) accept a tampered one.
 * These are the FIPS 180-4 / NIST example vectors, including the one-million
 * 'a' case that exercises the multi-block streaming path.
 */
#include "sha256.h"

#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;

static void check_hex(const char *label, const char *got, const char *want) {
    int ok = strcmp(got, want) == 0;
    printf("%s  %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) printf("      got  %s\n      want %s\n", got, want);
    ok ? g_pass++ : g_fail++;
}

/* Hash a one-shot buffer and compare against the expected hex digest. */
static void vector(const char *input, const char *want) {
    uint8_t digest[SHA256_DIGEST_SIZE];
    char     hex[65];
    char     label[256];

    sha256(input, strlen(input), digest);
    sha256_hex(digest, hex);
    snprintf(label, sizeof label, "sha256(\"%s\")", input);
    check_hex(label, hex, want);
}

int main(void) {
    /* FIPS 180-4 / NIST CAVP examples. */
    vector("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    vector("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    vector("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
           "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    /* One million 'a' characters, fed in 1000-byte chunks: this crosses many
     * 64-byte block boundaries and exercises the streaming path (the length
     * is encoded in the final block, which is the easy part to get wrong). */
    {
        sha256_ctx c;
        sha256_init(&c);
        char chunk[1000];
        memset(chunk, 'a', sizeof chunk);
        for (int i = 0; i < 1000; i++)
            sha256_update(&c, chunk, sizeof chunk);

        uint8_t digest[SHA256_DIGEST_SIZE];
        char     hex[65];
        sha256_final(&c, digest);
        sha256_hex(digest, hex);
        check_hex("sha256(1,000,000 x 'a') streamed",
                  hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    }

    /* Streaming in odd-sized pieces must equal the one-shot digest: proves the
     * buffering logic (not just the compression function) is correct. */
    {
        const char *msg = "the quick brown fox jumps over the lazy dog";
        sha256_ctx c;
        sha256_init(&c);
        for (size_t i = 0; i < strlen(msg); i++)
            sha256_update(&c, msg + i, 1);       /* one byte at a time */
        uint8_t d1[SHA256_DIGEST_SIZE]; char h1[65];
        sha256_final(&c, d1);
        sha256_hex(d1, h1);

        uint8_t d2[SHA256_DIGEST_SIZE]; char h2[65];
        sha256(msg, strlen(msg), d2);
        sha256_hex(d2, h2);

        check_hex("byte-at-a-time matches one-shot", h1, h2);
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}