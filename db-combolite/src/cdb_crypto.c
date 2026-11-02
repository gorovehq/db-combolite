/* Cryptographic primitives and security interfaces (spec 12, 14.1, 14.2.5). */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "combolite/cdb_crypto.h"
#include "cdb_endian.h"

#include "mbedtls/platform_util.h"
#include "mbedtls/gcm.h"
#include "mbedtls/chachapoly.h"
#include "mbedtls/sha256.h"
#include "mbedtls/md.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"

void cdb_crypto_wipe(void *buf, size_t len)
{
    if (buf != NULL && len > 0)
        mbedtls_platform_zeroize(buf, len);
}

void cdb_crypto_make_nonce(uint32_t track_id, uint64_t seq, uint8_t nonce[CDB_AEAD_NONCE_SIZE])
{
    /* Spec 14.2.5: track ID (4 bytes) || sequence number (8 bytes), stored big-endian. */
    cdb_store_u32(nonce, track_id, CDB_ORDER_BIG);
    cdb_store_u64(nonce + 4, seq, CDB_ORDER_BIG);
}

cdb_status cdb_crypto_aead_encrypt(unsigned alg, const uint8_t key[CDB_CRYPTO_KEY_SIZE],
                                   const uint8_t nonce[CDB_AEAD_NONCE_SIZE],
                                   const uint8_t *aad, size_t aad_len,
                                   const uint8_t *in_plaintext, size_t pt_len,
                                   uint8_t *out_ciphertext,
                                   uint8_t out_tag[CDB_AEAD_TAG_SIZE])
{
    int ret;
    if (key == NULL || nonce == NULL || out_tag == NULL)
        return CDB_E_INVALID_ARG;
    if (pt_len > 0 && (in_plaintext == NULL || out_ciphertext == NULL))
        return CDB_E_INVALID_ARG;

    if (alg == CDB_AEAD_AES_256_GCM) {
        mbedtls_gcm_context ctx;
        mbedtls_gcm_init(&ctx);
        ret = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256);
        if (ret == 0) {
            ret = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, pt_len,
                                           nonce, CDB_AEAD_NONCE_SIZE,
                                           aad, aad_len,
                                           in_plaintext, out_ciphertext,
                                           CDB_AEAD_TAG_SIZE, out_tag);
        }
        mbedtls_gcm_free(&ctx);
        return ret == 0 ? CDB_OK : CDB_E_CORRUPT;
    }

    if (alg == CDB_AEAD_CHACHA20_POLY1305) {
        mbedtls_chachapoly_context ctx;
        mbedtls_chachapoly_init(&ctx);
        ret = mbedtls_chachapoly_setkey(&ctx, key);
        if (ret == 0) {
            ret = mbedtls_chachapoly_encrypt_and_tag(&ctx, pt_len,
                                                    nonce,
                                                    aad, aad_len,
                                                    in_plaintext, out_ciphertext,
                                                    out_tag);
        }
        mbedtls_chachapoly_free(&ctx);
        return ret == 0 ? CDB_OK : CDB_E_CORRUPT;
    }

    return CDB_E_UNSUPPORTED;
}

cdb_status cdb_crypto_aead_decrypt(unsigned alg, const uint8_t key[CDB_CRYPTO_KEY_SIZE],
                                   const uint8_t nonce[CDB_AEAD_NONCE_SIZE],
                                   const uint8_t *aad, size_t aad_len,
                                   const uint8_t *in_ciphertext, size_t ct_len,
                                   const uint8_t tag[CDB_AEAD_TAG_SIZE],
                                   uint8_t *out_plaintext)
{
    int ret;
    if (key == NULL || nonce == NULL || tag == NULL)
        return CDB_E_INVALID_ARG;
    if (ct_len > 0 && (in_ciphertext == NULL || out_plaintext == NULL))
        return CDB_E_INVALID_ARG;

    if (alg == CDB_AEAD_AES_256_GCM) {
        mbedtls_gcm_context ctx;
        mbedtls_gcm_init(&ctx);
        ret = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256);
        if (ret == 0) {
            ret = mbedtls_gcm_auth_decrypt(&ctx, ct_len,
                                          nonce, CDB_AEAD_NONCE_SIZE,
                                          aad, aad_len,
                                          tag, CDB_AEAD_TAG_SIZE,
                                          in_ciphertext, out_plaintext);
        }
        mbedtls_gcm_free(&ctx);
        return ret == 0 ? CDB_OK : CDB_E_CORRUPT;
    }

    if (alg == CDB_AEAD_CHACHA20_POLY1305) {
        mbedtls_chachapoly_context ctx;
        mbedtls_chachapoly_init(&ctx);
        ret = mbedtls_chachapoly_setkey(&ctx, key);
        if (ret == 0) {
            ret = mbedtls_chachapoly_auth_decrypt(&ctx, ct_len,
                                                 nonce,
                                                 aad, aad_len,
                                                 tag,
                                                 in_ciphertext, out_plaintext);
        }
        mbedtls_chachapoly_free(&ctx);
        return ret == 0 ? CDB_OK : CDB_E_CORRUPT;
    }

    return CDB_E_UNSUPPORTED;
}

cdb_status cdb_crypto_sha256(const void *data, size_t len, uint8_t out_hash[CDB_CRYPTO_HASH_SIZE])
{
    int ret;
    if (out_hash == NULL || (len > 0 && data == NULL))
        return CDB_E_INVALID_ARG;
    ret = mbedtls_sha256((const unsigned char *)data, len, out_hash, 0);
    return ret == 0 ? CDB_OK : CDB_E_CORRUPT;
}

cdb_status cdb_crypto_hmac_sha256(const uint8_t key[CDB_CRYPTO_KEY_SIZE],
                                  const void *data, size_t len,
                                  uint8_t out_mac[CDB_CRYPTO_HASH_SIZE])
{
    const mbedtls_md_info_t *md;
    int ret;
    if (key == NULL || out_mac == NULL || (len > 0 && data == NULL))
        return CDB_E_INVALID_ARG;

    md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md == NULL)
        return CDB_E_UNSUPPORTED;

    ret = mbedtls_md_hmac(md, key, CDB_CRYPTO_KEY_SIZE, (const unsigned char *)data, len, out_mac);
    return ret == 0 ? CDB_OK : CDB_E_CORRUPT;
}

cdb_status cdb_crypto_hash_init(cdb_crypto_hash_ctx *ctx)
{
    mbedtls_sha256_context *s;
    if (ctx == NULL)
        return CDB_E_INVALID_ARG;
    if (sizeof(mbedtls_sha256_context) > sizeof(ctx->opaque))
        return CDB_E_STATE;

    memset(ctx, 0, sizeof *ctx);
    s = (mbedtls_sha256_context *)(void *)ctx->opaque;
    mbedtls_sha256_init(s);
    if (mbedtls_sha256_starts(s, 0) != 0)
        return CDB_E_CORRUPT;
    ctx->is_hmac = 0;
    return CDB_OK;
}

cdb_status cdb_crypto_hash_init_hmac(cdb_crypto_hash_ctx *ctx, const uint8_t key[CDB_CRYPTO_KEY_SIZE])
{
    mbedtls_sha256_context *s;
    uint8_t k_ipad[64];
    size_t i;
    if (ctx == NULL || key == NULL)
        return CDB_E_INVALID_ARG;
    if (sizeof(mbedtls_sha256_context) > sizeof(ctx->opaque))
        return CDB_E_STATE;

    memset(ctx, 0, sizeof *ctx);
    for (i = 0; i < 64; i++) {
        uint8_t k = (i < CDB_CRYPTO_KEY_SIZE) ? key[i] : 0u;
        k_ipad[i] = (uint8_t)(k ^ 0x36u);
        ctx->opad[i] = (uint8_t)(k ^ 0x5Cu);
    }

    s = (mbedtls_sha256_context *)(void *)ctx->opaque;
    mbedtls_sha256_init(s);
    if (mbedtls_sha256_starts(s, 0) != 0 || mbedtls_sha256_update(s, k_ipad, 64) != 0) {
        mbedtls_platform_zeroize(k_ipad, sizeof k_ipad);
        return CDB_E_CORRUPT;
    }
    mbedtls_platform_zeroize(k_ipad, sizeof k_ipad);
    ctx->is_hmac = 1;
    return CDB_OK;
}

cdb_status cdb_crypto_hash_update(cdb_crypto_hash_ctx *ctx, const void *data, size_t len)
{
    mbedtls_sha256_context *s;
    if (ctx == NULL || (len > 0 && data == NULL))
        return CDB_E_INVALID_ARG;
    if (len == 0)
        return CDB_OK;

    s = (mbedtls_sha256_context *)(void *)ctx->opaque;
    if (mbedtls_sha256_update(s, (const unsigned char *)data, len) != 0)
        return CDB_E_CORRUPT;
    return CDB_OK;
}

cdb_status cdb_crypto_hash_final(cdb_crypto_hash_ctx *ctx, uint8_t out_hash[CDB_CRYPTO_HASH_SIZE])
{
    mbedtls_sha256_context *s;
    if (ctx == NULL || out_hash == NULL)
        return CDB_E_INVALID_ARG;

    s = (mbedtls_sha256_context *)(void *)ctx->opaque;
    if (!ctx->is_hmac) {
        if (mbedtls_sha256_finish(s, out_hash) != 0)
            return CDB_E_CORRUPT;
    } else {
        uint8_t inner[CDB_CRYPTO_HASH_SIZE];
        if (mbedtls_sha256_finish(s, inner) != 0)
            return CDB_E_CORRUPT;
        if (mbedtls_sha256_starts(s, 0) != 0 ||
            mbedtls_sha256_update(s, ctx->opad, 64) != 0 ||
            mbedtls_sha256_update(s, inner, sizeof inner) != 0 ||
            mbedtls_sha256_finish(s, out_hash) != 0) {
            mbedtls_platform_zeroize(inner, sizeof inner);
            return CDB_E_CORRUPT;
        }
        mbedtls_platform_zeroize(inner, sizeof inner);
    }
    return CDB_OK;
}

void cdb_crypto_hash_free(cdb_crypto_hash_ctx *ctx)
{
    if (ctx != NULL) {
        mbedtls_sha256_context *s = (mbedtls_sha256_context *)(void *)ctx->opaque;
        mbedtls_sha256_free(s);
        mbedtls_platform_zeroize(ctx, sizeof *ctx);
    }
}

cdb_status cdb_crypto_derive_subkey(const uint8_t base_key[CDB_CRYPTO_KEY_SIZE],
                                    cdb_subkey_purpose purpose,
                                    uint8_t subkey_out[CDB_CRYPTO_KEY_SIZE])
{
    const mbedtls_md_info_t *md;
    const char *info_str;
    int ret;

    if (base_key == NULL || subkey_out == NULL)
        return CDB_E_INVALID_ARG;

    switch (purpose) {
    case CDB_SUBKEY_PAGE:
        info_str = "combolite:page";
        break;
    case CDB_SUBKEY_CONTENT_HASH:
        info_str = "combolite:hash";
        break;
    case CDB_SUBKEY_INDEX:
        info_str = "combolite:index";
        break;
    case CDB_SUBKEY_INTEGRITY:
        info_str = "combolite:integrity";
        break;
    default:
        return CDB_E_INVALID_ARG;
    }

    md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md == NULL)
        return CDB_E_UNSUPPORTED;

    ret = mbedtls_hkdf_expand(md, base_key, CDB_CRYPTO_KEY_SIZE,
                              (const unsigned char *)info_str, strlen(info_str),
                              subkey_out, CDB_CRYPTO_KEY_SIZE);
    return ret == 0 ? CDB_OK : CDB_E_CORRUPT;
}

cdb_status cdb_crypto_wrap_key(const uint8_t master_key[CDB_CRYPTO_KEY_SIZE],
                               const uint8_t enclave_key[CDB_CRYPTO_KEY_SIZE],
                               uint8_t wrapped_out[CDB_WRAPPED_KEY_SIZE])
{
    uint8_t nonce[CDB_AEAD_NONCE_SIZE];
    uint8_t tag[CDB_AEAD_TAG_SIZE];
    cdb_status s;

    if (master_key == NULL || enclave_key == NULL || wrapped_out == NULL)
        return CDB_E_INVALID_ARG;

    /* Wrap using AES-256-GCM with a dedicated wrap track ID. */
    cdb_crypto_make_nonce(0x57524150u /* "WRAP" */, 1, nonce);
    s = cdb_crypto_aead_encrypt(CDB_AEAD_AES_256_GCM, master_key, nonce,
                                NULL, 0,
                                enclave_key, CDB_CRYPTO_KEY_SIZE,
                                wrapped_out + CDB_AEAD_TAG_SIZE,
                                tag);
    if (s != CDB_OK)
        return s;

    memcpy(wrapped_out, tag, CDB_AEAD_TAG_SIZE);
    return CDB_OK;
}

cdb_status cdb_crypto_unwrap_key(const uint8_t master_key[CDB_CRYPTO_KEY_SIZE],
                                 const uint8_t wrapped_in[CDB_WRAPPED_KEY_SIZE],
                                 uint8_t enclave_key_out[CDB_CRYPTO_KEY_SIZE])
{
    uint8_t nonce[CDB_AEAD_NONCE_SIZE];

    if (master_key == NULL || wrapped_in == NULL || enclave_key_out == NULL)
        return CDB_E_INVALID_ARG;

    cdb_crypto_make_nonce(0x57524150u /* "WRAP" */, 1, nonce);
    return cdb_crypto_aead_decrypt(CDB_AEAD_AES_256_GCM, master_key, nonce,
                                   NULL, 0,
                                   wrapped_in + CDB_AEAD_TAG_SIZE, CDB_CRYPTO_KEY_SIZE,
                                   wrapped_in,
                                   enclave_key_out);
}

/* Parses 32 bytes from either a 64-hex-character string or raw bytes. */
static int parse_key_bytes(const char *src, size_t len, uint8_t out[CDB_CRYPTO_KEY_SIZE])
{
    size_t i;
    int is_hex = 1;

    if (len >= 64) {
        for (i = 0; i < 64; ++i) {
            char c = src[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                is_hex = 0;
                break;
            }
        }
        if (is_hex) {
            for (i = 0; i < 32; ++i) {
                unsigned v = 0;
                char hex[3];
                hex[0] = src[2 * i];
                hex[1] = src[2 * i + 1];
                hex[2] = '\0';
                if (sscanf(hex, "%02x", &v) != 1)
                    return 0;
                out[i] = (uint8_t)v;
            }
            return 1;
        }
    }

    if (len >= CDB_CRYPTO_KEY_SIZE) {
        memcpy(out, src, CDB_CRYPTO_KEY_SIZE);
        return 1;
    }

    return 0;
}

cdb_status cdb_crypto_resolve_key(const char *uri, int is_production,
                                  cdb_key_provider_fn custom_provider, void *user_data,
                                  uint8_t key_out[CDB_CRYPTO_KEY_SIZE])
{
    if (uri == NULL || key_out == NULL)
        return CDB_E_INVALID_ARG;

    if (strncmp(uri, "env://", 6) == 0) {
        const char *var_name = uri + 6;
        const char *val = getenv(var_name);
        if (val == NULL)
            return CDB_E_NOT_FOUND;
        if (!parse_key_bytes(val, strlen(val), key_out))
            return CDB_E_MALFORMED;
        return CDB_OK;
    }

    if (strncmp(uri, "file://", 7) == 0) {
        const char *path = uri + 7;
        char buf[128];
        size_t nread;
        FILE *f;

        if (is_production) {
            fprintf(stderr, "combolite: ERROR: file:// key provider is prohibited in production mode! (spec 12.3)\n");
            return CDB_E_SECURITY;
        }

        fprintf(stderr, "combolite: WARNING: file:// key provider is for development only! Do not use in production. (spec 12.3)\n");

        f = fopen(path, "rb");
        if (f == NULL)
            return CDB_E_NOT_FOUND;

        nread = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);

        while (nread > 0 && (buf[nread - 1] == '\r' || buf[nread - 1] == '\n' || buf[nread - 1] == ' '))
            nread--;

        if (!parse_key_bytes(buf, nread, key_out)) {
            cdb_crypto_wipe(buf, sizeof(buf));
            return CDB_E_MALFORMED;
        }
        cdb_crypto_wipe(buf, sizeof(buf));
        return CDB_OK;
    }

    if (custom_provider != NULL)
        return custom_provider(uri, key_out, user_data);

    return CDB_E_UNSUPPORTED;
}

/* Basic entropy source reading from /dev/urandom. */
static int platform_entropy_rng(void *ctx, unsigned char *buf, size_t len)
{
    FILE *f;
    size_t n;
    (void)ctx;
    f = fopen("/dev/urandom", "rb");
    if (f == NULL)
        return MBEDTLS_ERR_ECP_RANDOM_FAILED;
    n = fread(buf, 1, len, f);
    fclose(f);
    return n == len ? 0 : MBEDTLS_ERR_ECP_RANDOM_FAILED;
}

cdb_status cdb_crypto_random_bytes(void *buf, size_t len)
{
    if (buf == NULL)
        return CDB_E_INVALID_ARG;
    return platform_entropy_rng(NULL, (unsigned char *)buf, len) == 0 ? CDB_OK : CDB_E_IO;
}

cdb_status cdb_identity_generate(cdb_identity *out_id, uint64_t principal_id, uint32_t tenant_id)
{
    mbedtls_ecdsa_context ctx;
    size_t olen = 0;
    int ret;

    if (out_id == NULL)
        return CDB_E_INVALID_ARG;

    memset(out_id, 0, sizeof(*out_id));
    out_id->principal_id = principal_id;
    out_id->tenant_id = tenant_id;

    mbedtls_ecdsa_init(&ctx);
    ret = mbedtls_ecdsa_genkey(&ctx, MBEDTLS_ECP_DP_SECP256R1, platform_entropy_rng, NULL);
    if (ret != 0) {
        mbedtls_ecdsa_free(&ctx);
        return CDB_E_CORRUPT;
    }

    ret = mbedtls_mpi_write_binary(&ctx.MBEDTLS_PRIVATE(d), out_id->secret_key, CDB_IDENTITY_SECKEY_SIZE);
    if (ret == 0) {
        ret = mbedtls_ecp_point_write_binary(&ctx.MBEDTLS_PRIVATE(grp),
                                             &ctx.MBEDTLS_PRIVATE(Q),
                                             MBEDTLS_ECP_PF_COMPRESSED,
                                             &olen,
                                             out_id->public_key,
                                             CDB_IDENTITY_PUBKEY_SIZE);
    }

    mbedtls_ecdsa_free(&ctx);
    return (ret == 0 && olen == CDB_IDENTITY_PUBKEY_SIZE) ? CDB_OK : CDB_E_CORRUPT;
}

cdb_status cdb_identity_sign(const cdb_identity *id, const void *msg, size_t msg_len,
                             uint8_t sig[CDB_SIGNATURE_SIZE])
{
    mbedtls_ecp_group grp;
    mbedtls_mpi r, s, d;
    uint8_t hash[CDB_CRYPTO_HASH_SIZE];
    cdb_status status;
    int ret;

    if (id == NULL || sig == NULL || (msg_len > 0 && msg == NULL))
        return CDB_E_INVALID_ARG;

    status = cdb_crypto_sha256(msg, msg_len, hash);
    if (status != CDB_OK)
        return status;

    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    mbedtls_mpi_init(&d);

    ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (ret == 0)
        ret = mbedtls_mpi_read_binary(&d, id->secret_key, CDB_IDENTITY_SECKEY_SIZE);

    if (ret == 0) {
        /* Deterministic ECDSA signature (RFC 6979). */
        ret = mbedtls_ecdsa_sign_det_ext(&grp, &r, &s, &d, hash, CDB_CRYPTO_HASH_SIZE,
                                        MBEDTLS_MD_SHA256, platform_entropy_rng, NULL);
    }

    if (ret == 0) {
        ret = mbedtls_mpi_write_binary(&r, sig, 32);
        if (ret == 0)
            ret = mbedtls_mpi_write_binary(&s, sig + 32, 32);
    }

    mbedtls_mpi_free(&d);
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_group_free(&grp);
    cdb_crypto_wipe(hash, sizeof(hash));

    return ret == 0 ? CDB_OK : CDB_E_CORRUPT;
}

cdb_status cdb_identity_verify(const uint8_t public_key[CDB_IDENTITY_PUBKEY_SIZE],
                               const void *msg, size_t msg_len,
                               const uint8_t sig[CDB_SIGNATURE_SIZE])
{
    mbedtls_ecp_group grp;
    mbedtls_ecp_point Q;
    mbedtls_mpi r, s;
    uint8_t hash[CDB_CRYPTO_HASH_SIZE];
    cdb_status status;
    int ret;

    if (public_key == NULL || sig == NULL || (msg_len > 0 && msg == NULL))
        return CDB_E_INVALID_ARG;

    status = cdb_crypto_sha256(msg, msg_len, hash);
    if (status != CDB_OK)
        return status;

    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&Q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (ret == 0)
        ret = mbedtls_ecp_point_read_binary(&grp, &Q, public_key, CDB_IDENTITY_PUBKEY_SIZE);
    if (ret == 0)
        ret = mbedtls_mpi_read_binary(&r, sig, 32);
    if (ret == 0)
        ret = mbedtls_mpi_read_binary(&s, sig + 32, 32);

    if (ret == 0)
        ret = mbedtls_ecdsa_verify(&grp, hash, CDB_CRYPTO_HASH_SIZE, &Q, &r, &s);

    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&Q);
    mbedtls_ecp_group_free(&grp);

    return ret == 0 ? CDB_OK : CDB_E_CORRUPT;
}
