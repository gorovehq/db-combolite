/* Cryptographic primitives and security interfaces (spec 12, 14.1, 14.2.5).
 *
 * Provides authenticated encryption (AES-256-GCM and ChaCha20-Poly1305),
 * deterministic nonces, key derivation, envelope wrapping, key providers,
 * tamper-evident tagging, and principal identity signatures. */
#ifndef CDB_CRYPTO_H
#define CDB_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#include "combolite/cdb_common.h"

/* AEAD Algorithm IDs (file header offset 77, spec 12.1). */
#define CDB_AEAD_NONE 0u
#define CDB_AEAD_AES_256_GCM 1u
#define CDB_AEAD_CHACHA20_POLY1305 2u

#define CDB_CRYPTO_KEY_SIZE 32u
#define CDB_AEAD_NONCE_SIZE 12u
#define CDB_AEAD_TAG_SIZE 16u
#define CDB_CRYPTO_HASH_SIZE 32u

/* Nonce track IDs (spec 14.2.5). */
#define CDB_TRACK_ARCHIVE_PAGE 0xFFFFFFFFu

/* Subkey purposes (spec 12.2): one subkey per job, each derived from a
 * domain key (or, for integrity, the master key) with HKDF-SHA-256 and the
 * job's label as the info. */
typedef enum cdb_subkey_purpose {
    CDB_SUBKEY_PAGE = 1,      /* "cdb page": pages and delta records; takes the session salt */
    CDB_SUBKEY_CONTENT_HASH,  /* "cdb hash": keyed content hashes */
    CDB_SUBKEY_INDEX,         /* "cdb index": the domain's indexes, joined-store tags */
    CDB_SUBKEY_INTEGRITY,     /* "cdb integrity": tamper-evident tags, from the master key (12.7) */
    CDB_SUBKEY_SEAL,          /* "cdb seal": sealed references in joined stores */
    CDB_SUBKEY_SEAL_NONCE,    /* "cdb seal nonce": their synthetic nonces */
    CDB_SUBKEY_REPL,          /* "cdb repl": sealed replication changes */
    CDB_SUBKEY_REPL_NONCE     /* "cdb repl nonce": their synthetic nonces */
} cdb_subkey_purpose;

#define CDB_SESSION_SALT_SIZE 16u

/* Overdue key rotation actions (spec 12.4). */
typedef enum cdb_rotation_policy {
    CDB_ROTATION_WARN = 0,
    CDB_ROTATION_REFUSE_WRITES = 1,
    CDB_ROTATION_REFUSE_OPEN = 2
} cdb_rotation_policy;

/* RBAC permissions mask (spec 12.5). */
#define CDB_PERM_NONE  0x00u
#define CDB_PERM_READ  0x01u
#define CDB_PERM_WRITE 0x02u
#define CDB_PERM_EXEC  0x04u
#define CDB_PERM_ADMIN 0x08u
#define CDB_PERM_ALL   (CDB_PERM_READ | CDB_PERM_WRITE | CDB_PERM_EXEC | CDB_PERM_ADMIN)

/* Signing policy for revisions and audit records (spec 12.5). */
typedef enum cdb_signing_policy {
    CDB_SIGN_NONE = 0,
    CDB_SIGN_AUDIT_ONLY = 1, /* Default (spec 12.5) */
    CDB_SIGN_EVERY_REVISION = 2
} cdb_signing_policy;

/* Key provider callback function type (spec 12.3).
 * uri: null-terminated URI (e.g. "vault://secret/key#1").
 * key_out: caller-provided 32-byte buffer for the raw master/enclave key. */
typedef cdb_status (*cdb_key_provider_fn)(const char *uri, uint8_t key_out[CDB_CRYPTO_KEY_SIZE],
                                          void *user_data);

/* Asymmetric key identity (spec 12.5): 33-byte compressed public key, 32-byte secret key. */
#define CDB_IDENTITY_PUBKEY_SIZE 33u
#define CDB_IDENTITY_SECKEY_SIZE 32u
#define CDB_SIGNATURE_SIZE 64u

typedef struct cdb_identity {
    uint8_t public_key[CDB_IDENTITY_PUBKEY_SIZE];
    uint8_t secret_key[CDB_IDENTITY_SECKEY_SIZE]; /* zeroed if public-only */
    uint64_t principal_id;
    uint32_t tenant_id;
} cdb_identity;

/* Securely zeroes memory (spec 12.6). */
void cdb_crypto_wipe(void *buf, size_t len);

/* Fills buf with cryptographically secure random bytes. */
cdb_status cdb_crypto_random_bytes(void *buf, size_t len);

/* Derives a 12-byte deterministic nonce: track_id (4B, big-endian) || seq (8B, big-endian). */
void cdb_crypto_make_nonce(uint32_t track_id, uint64_t seq, uint8_t nonce[CDB_AEAD_NONCE_SIZE]);

/* Authenticated encryption: encrypts plaintext into ciphertext and writes a 16-byte tag.
 * out_ciphertext may alias in_plaintext. */
cdb_status cdb_crypto_aead_encrypt(unsigned alg, const uint8_t key[CDB_CRYPTO_KEY_SIZE],
                                   const uint8_t nonce[CDB_AEAD_NONCE_SIZE],
                                   const uint8_t *aad, size_t aad_len,
                                   const uint8_t *in_plaintext, size_t pt_len,
                                   uint8_t *out_ciphertext,
                                   uint8_t out_tag[CDB_AEAD_TAG_SIZE]);

/* Authenticated decryption: decrypts ciphertext and verifies the 16-byte tag.
 * Returns CDB_E_CORRUPT if authentication fails. out_plaintext may alias in_ciphertext. */
cdb_status cdb_crypto_aead_decrypt(unsigned alg, const uint8_t key[CDB_CRYPTO_KEY_SIZE],
                                   const uint8_t nonce[CDB_AEAD_NONCE_SIZE],
                                   const uint8_t *aad, size_t aad_len,
                                   const uint8_t *in_ciphertext, size_t ct_len,
                                   const uint8_t tag[CDB_AEAD_TAG_SIZE],
                                   uint8_t *out_plaintext);

/* SHA-256 and HMAC-SHA-256 helpers. */
cdb_status cdb_crypto_sha256(const void *data, size_t len, uint8_t out_hash[CDB_CRYPTO_HASH_SIZE]);

cdb_status cdb_crypto_hmac_sha256(const uint8_t key[CDB_CRYPTO_KEY_SIZE],
                                  const void *data, size_t len,
                                  uint8_t out_mac[CDB_CRYPTO_HASH_SIZE]);

/* Streaming hash context (SHA-256 or HMAC-SHA-256). Fixed size, zero heap allocation. */
typedef struct cdb_crypto_hash_ctx {
    uint32_t opaque[32]; /* 128 bytes, aligned, holds internal sha context */
    uint8_t opad[64];    /* outer pad for HMAC if enabled */
    int is_hmac;
} cdb_crypto_hash_ctx;

cdb_status cdb_crypto_hash_init(cdb_crypto_hash_ctx *ctx);
cdb_status cdb_crypto_hash_init_hmac(cdb_crypto_hash_ctx *ctx, const uint8_t key[CDB_CRYPTO_KEY_SIZE]);
cdb_status cdb_crypto_hash_update(cdb_crypto_hash_ctx *ctx, const void *data, size_t len);
cdb_status cdb_crypto_hash_final(cdb_crypto_hash_ctx *ctx, uint8_t out_hash[CDB_CRYPTO_HASH_SIZE]);
void cdb_crypto_hash_free(cdb_crypto_hash_ctx *ctx);

/* Purpose-separated subkey derivation via HKDF-SHA-256 (spec 12.2, 14.2.8).
 * salt is the 16-byte session salt, used as the HKDF salt; only the page
 * subkey takes one, and the others are derived with an empty salt so that
 * every copy and session computes them alike. CDB_E_INVALID_ARG for a salt
 * given to any other purpose. */
cdb_status cdb_crypto_derive_subkey(const uint8_t base_key[CDB_CRYPTO_KEY_SIZE],
                                    cdb_subkey_purpose purpose,
                                    const uint8_t *salt,
                                    uint8_t subkey_out[CDB_CRYPTO_KEY_SIZE]);

/* Key wrapping for envelope encryption (spec 12.2):
 * Wraps a 32-byte enclave key using the 32-byte master key into a 48-byte wrapped buffer
 * (16-byte tag + 32-byte ciphertext). */
#define CDB_WRAPPED_KEY_SIZE (CDB_CRYPTO_KEY_SIZE + CDB_AEAD_TAG_SIZE)

cdb_status cdb_crypto_wrap_key(const uint8_t master_key[CDB_CRYPTO_KEY_SIZE],
                               const uint8_t enclave_key[CDB_CRYPTO_KEY_SIZE],
                               uint8_t wrapped_out[CDB_WRAPPED_KEY_SIZE]);

cdb_status cdb_crypto_unwrap_key(const uint8_t master_key[CDB_CRYPTO_KEY_SIZE],
                                 const uint8_t wrapped_in[CDB_WRAPPED_KEY_SIZE],
                                 uint8_t enclave_key_out[CDB_CRYPTO_KEY_SIZE]);

/* Key provider URI resolution (spec 12.3).
 * Resolves env://, file:// (with warnings and is_production refusal), and registered hooks. */
cdb_status cdb_crypto_resolve_key(const char *uri, int is_production,
                                  cdb_key_provider_fn custom_provider, void *user_data,
                                  uint8_t key_out[CDB_CRYPTO_KEY_SIZE]);

/* Principal identity generation, signing, and verification (spec 12.5). */
cdb_status cdb_identity_generate(cdb_identity *out_id, uint64_t principal_id, uint32_t tenant_id);

cdb_status cdb_identity_sign(const cdb_identity *id, const void *msg, size_t msg_len,
                             uint8_t sig[CDB_SIGNATURE_SIZE]);

cdb_status cdb_identity_verify(const uint8_t public_key[CDB_IDENTITY_PUBKEY_SIZE],
                               const void *msg, size_t msg_len,
                               const uint8_t sig[CDB_SIGNATURE_SIZE]);

#endif