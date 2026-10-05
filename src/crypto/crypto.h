/* SPDX-License-Identifier: GPL-2.0 */
/*
 * crypto.h — the kernel's own cryptographic primitives. (GPLv2)
 *
 * Three things every subsystem eventually needs, implemented once and
 * verified against published test vectors at boot:
 *   - SHA-256 (FIPS 180-4): integrity, key derivation, /dev/hash-style use
 *   - CRC-32 (IEEE 802.3): checksums for images and on-the-wire metadata
 *   - AES-128 (FIPS 197): the block primitive future storage encryption
 *     and secure-boot-style verification build on
 *
 * Everything is constant-time where it matters (AES S-box is table-driven
 * on public data only; no secret-dependent branches in the block cipher).
 */
#ifndef GNUCOS_CRYPTO_H
#define GNUCOS_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

/* CRC-32 (IEEE), table-driven.  crc starts at 0 for a fresh message. */
uint32_t crypto_crc32(const void *data, size_t len);

/* SHA-256: one-shot.  `out` receives 32 bytes. */
void crypto_sha256(const void *data, size_t len, uint8_t out[32]);
/* Streaming form: init/update per chunk, final pads and emits. */
typedef struct {
    uint32_t h[8];
    uint64_t total; /* bytes fed so far */
    uint8_t  buf[64];
    uint32_t buflen;
} sha256_ctx_t;
void sha256_init(sha256_ctx_t *ctx);
void sha256_update(sha256_ctx_t *ctx, const void *data, size_t len);
void sha256_final(sha256_ctx_t *ctx, uint8_t out[32]);

/* AES-128: encrypt/decrypt one 16-byte block with a 16-byte key. */
void crypto_aes128_expand_key(const uint8_t key[16], uint32_t roundkeys[44]);
void crypto_aes128_encrypt_block(const uint32_t roundkeys[44], const uint8_t in[16],
                                 uint8_t out[16]);
void crypto_aes128_decrypt_block(const uint32_t roundkeys[44], const uint8_t in[16],
                                 uint8_t out[16]);

/* Boot-time known-answer tests; prints CRYPTO: self-test PASS/FAIL. */
void crypto_selftest(void);

#endif
