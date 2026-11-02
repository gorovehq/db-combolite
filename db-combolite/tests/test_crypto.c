#define _POSIX_C_SOURCE 200809L
/* Unit tests for cryptographic primitives and security interfaces (spec 12). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "combolite/cdb_crypto.h"
#include "cdb_endian.h"

void setUp(void) {}
void tearDown(void) {}

static void test_make_nonce(void)
{
    uint8_t nonce[CDB_AEAD_NONCE_SIZE];
    cdb_crypto_make_nonce(0x01020304u, 0x1122334455667788ULL, nonce);

    /* Verify big-endian byte layout: 4 bytes track ID, 8 bytes seq */
    TEST_ASSERT_EQUAL_HEX8(0x01, nonce[0]);
    TEST_ASSERT_EQUAL_HEX8(0x02, nonce[1]);
    TEST_ASSERT_EQUAL_HEX8(0x03, nonce[2]);
    TEST_ASSERT_EQUAL_HEX8(0x04, nonce[3]);

    TEST_ASSERT_EQUAL_HEX8(0x11, nonce[4]);
    TEST_ASSERT_EQUAL_HEX8(0x22, nonce[5]);
    TEST_ASSERT_EQUAL_HEX8(0x33, nonce[6]);
    TEST_ASSERT_EQUAL_HEX8(0x44, nonce[7]);
    TEST_ASSERT_EQUAL_HEX8(0x55, nonce[8]);
    TEST_ASSERT_EQUAL_HEX8(0x66, nonce[9]);
    TEST_ASSERT_EQUAL_HEX8(0x77, nonce[10]);
    TEST_ASSERT_EQUAL_HEX8(0x88, nonce[11]);
}

static void test_aead_aes_gcm_roundtrip(void)
{
    uint8_t key[CDB_CRYPTO_KEY_SIZE];
    uint8_t wrong_key[CDB_CRYPTO_KEY_SIZE];
    uint8_t nonce[CDB_AEAD_NONCE_SIZE];
    uint8_t tag[CDB_AEAD_TAG_SIZE];
    const char *aad_str = "combolite_page_header_aad";
    const char *plain_str = "Confidential data payload across database pages.";
    size_t aad_len = strlen(aad_str);
    size_t pt_len = strlen(plain_str) + 1;
    uint8_t ciphertext[128];
    uint8_t decrypted[128];

    memset(key, 0x42, sizeof(key));
    memset(wrong_key, 0x43, sizeof(wrong_key));
    cdb_crypto_make_nonce(CDB_TRACK_ARCHIVE_PAGE, 100, nonce);

    /* Encrypt */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crypto_aead_encrypt(CDB_AEAD_AES_256_GCM, key, nonce,
                                                          (const uint8_t *)aad_str, aad_len,
                                                          (const uint8_t *)plain_str, pt_len,
                                                          ciphertext, tag));

    /* Ciphertext must not match plaintext */
    TEST_ASSERT_NOT_EQUAL(0, memcmp(plain_str, ciphertext, pt_len));

    /* Decrypt successfully */
    memset(decrypted, 0, sizeof(decrypted));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crypto_aead_decrypt(CDB_AEAD_AES_256_GCM, key, nonce,
                                                          (const uint8_t *)aad_str, aad_len,
                                                          ciphertext, pt_len,
                                                          tag, decrypted));
    TEST_ASSERT_EQUAL_STRING(plain_str, (char *)decrypted);

    /* Decrypt with wrong key fails */
    TEST_ASSERT_EQUAL_INT(CDB_E_CORRUPT, cdb_crypto_aead_decrypt(CDB_AEAD_AES_256_GCM, wrong_key, nonce,
                                                                 (const uint8_t *)aad_str, aad_len,
                                                                 ciphertext, pt_len,
                                                                 tag, decrypted));

    /* Decrypt with corrupted tag fails */
    tag[0] ^= 0x01;
    TEST_ASSERT_EQUAL_INT(CDB_E_CORRUPT, cdb_crypto_aead_decrypt(CDB_AEAD_AES_256_GCM, key, nonce,
                                                                 (const uint8_t *)aad_str, aad_len,
                                                                 ciphertext, pt_len,
                                                                 tag, decrypted));
    tag[0] ^= 0x01;

    /* Decrypt with corrupted ciphertext fails */
    ciphertext[0] ^= 0x01;
    TEST_ASSERT_EQUAL_INT(CDB_E_CORRUPT, cdb_crypto_aead_decrypt(CDB_AEAD_AES_256_GCM, key, nonce,
                                                                 (const uint8_t *)aad_str, aad_len,
                                                                 ciphertext, pt_len,
                                                                 tag, decrypted));
    ciphertext[0] ^= 0x01;

    /* Decrypt with corrupted AAD fails */
    TEST_ASSERT_EQUAL_INT(CDB_E_CORRUPT, cdb_crypto_aead_decrypt(CDB_AEAD_AES_256_GCM, key, nonce,
                                                                 (const uint8_t *)"corrupted_aad", 13,
                                                                 ciphertext, pt_len,
                                                                 tag, decrypted));
}

static void test_aead_chacha20_poly1305_roundtrip(void)
{
    uint8_t key[CDB_CRYPTO_KEY_SIZE];
    uint8_t nonce[CDB_AEAD_NONCE_SIZE];
    uint8_t tag[CDB_AEAD_TAG_SIZE];
    const char *plain_str = "Poly1305 authenticated encryption test message.";
    size_t pt_len = strlen(plain_str) + 1;
    uint8_t ciphertext[128];
    uint8_t decrypted[128];

    memset(key, 0x77, sizeof(key));
    cdb_crypto_make_nonce(0x01, 55, nonce);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crypto_aead_encrypt(CDB_AEAD_CHACHA20_POLY1305, key, nonce,
                                                          (const uint8_t *)"aad", 3,
                                                          (const uint8_t *)plain_str, pt_len,
                                                          ciphertext, tag));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crypto_aead_decrypt(CDB_AEAD_CHACHA20_POLY1305, key, nonce,
                                                          (const uint8_t *)"aad", 3,
                                                          ciphertext, pt_len,
                                                          tag, decrypted));
    TEST_ASSERT_EQUAL_STRING(plain_str, (char *)decrypted);

    /* Corrupted tag */
    tag[15] ^= 0x80;
    TEST_ASSERT_EQUAL_INT(CDB_E_CORRUPT, cdb_crypto_aead_decrypt(CDB_AEAD_CHACHA20_POLY1305, key, nonce,
                                                                 (const uint8_t *)"aad", 3,
                                                                 ciphertext, pt_len,
                                                                 tag, decrypted));
}

static void test_sha256_and_hmac(void)
{
    uint8_t hash[CDB_CRYPTO_HASH_SIZE];
    uint8_t mac1[CDB_CRYPTO_HASH_SIZE];
    uint8_t mac2[CDB_CRYPTO_HASH_SIZE];
    uint8_t key1[CDB_CRYPTO_KEY_SIZE];
    uint8_t key2[CDB_CRYPTO_KEY_SIZE];
    const char *msg = "The quick brown fox jumps over the lazy dog";

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crypto_sha256(msg, strlen(msg), hash));
    /* Known SHA-256 for "The quick brown fox jumps over the lazy dog":
     * d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592 */
    TEST_ASSERT_EQUAL_HEX8(0xd7, hash[0]);
    TEST_ASSERT_EQUAL_HEX8(0xa8, hash[1]);
    TEST_ASSERT_EQUAL_HEX8(0x92, hash[31]);

    memset(key1, 0x11, sizeof(key1));
    memset(key2, 0x22, sizeof(key2));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crypto_hmac_sha256(key1, msg, strlen(msg), mac1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crypto_hmac_sha256(key2, msg, strlen(msg), mac2));

    /* Different keys yield different HMACs */
    TEST_ASSERT_NOT_EQUAL(0, memcmp(mac1, mac2, CDB_CRYPTO_HASH_SIZE));
}

static void test_subkey_derivation(void)
{
    uint8_t base_key[CDB_CRYPTO_KEY_SIZE];
    uint8_t page_key[CDB_CRYPTO_KEY_SIZE];
    uint8_t hash_key[CDB_CRYPTO_KEY_SIZE];
    uint8_t idx_key[CDB_CRYPTO_KEY_SIZE];
    uint8_t integ_key[CDB_CRYPTO_KEY_SIZE];

    memset(base_key, 0xA5, sizeof(base_key));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crypto_derive_subkey(base_key, CDB_SUBKEY_PAGE, page_key));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crypto_derive_subkey(base_key, CDB_SUBKEY_CONTENT_HASH, hash_key));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crypto_derive_subkey(base_key, CDB_SUBKEY_INDEX, idx_key));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crypto_derive_subkey(base_key, CDB_SUBKEY_INTEGRITY, integ_key));

    /* All derived subkeys must be mutually distinct */
    TEST_ASSERT_NOT_EQUAL(0, memcmp(page_key, hash_key, CDB_CRYPTO_KEY_SIZE));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(page_key, idx_key, CDB_CRYPTO_KEY_SIZE));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(page_key, integ_key, CDB_CRYPTO_KEY_SIZE));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(hash_key, idx_key, CDB_CRYPTO_KEY_SIZE));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(base_key, page_key, CDB_CRYPTO_KEY_SIZE));
}

static void test_key_wrapping(void)
{
    uint8_t master[CDB_CRYPTO_KEY_SIZE];
    uint8_t enclave[CDB_CRYPTO_KEY_SIZE];
    uint8_t wrapped[CDB_WRAPPED_KEY_SIZE];
    uint8_t recovered[CDB_CRYPTO_KEY_SIZE];
    uint8_t wrong_master[CDB_CRYPTO_KEY_SIZE];

    memset(master, 0x12, sizeof(master));
    memset(enclave, 0x34, sizeof(enclave));
    memset(wrong_master, 0x56, sizeof(wrong_master));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crypto_wrap_key(master, enclave, wrapped));

    /* Recover with master key */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crypto_unwrap_key(master, wrapped, recovered));
    TEST_ASSERT_EQUAL_MEMORY(enclave, recovered, CDB_CRYPTO_KEY_SIZE);

    /* Wrong master key fails */
    TEST_ASSERT_EQUAL_INT(CDB_E_CORRUPT, cdb_crypto_unwrap_key(wrong_master, wrapped, recovered));

    /* Corrupted wrapped payload fails */
    wrapped[5] ^= 0xFF;
    TEST_ASSERT_EQUAL_INT(CDB_E_CORRUPT, cdb_crypto_unwrap_key(master, wrapped, recovered));
}

static cdb_status custom_mock_provider(const char *uri, uint8_t key_out[CDB_CRYPTO_KEY_SIZE], void *user_data)
{
    (void)user_data;
    if (strcmp(uri, "vault://secret/app/db") == 0) {
        memset(key_out, 0xEE, CDB_CRYPTO_KEY_SIZE);
        return CDB_OK;
    }
    return CDB_E_NOT_FOUND;
}

static void test_key_providers(void)
{
    uint8_t key[CDB_CRYPTO_KEY_SIZE];
    FILE *f;
    const char *tmp_keyfile = CDB_TEST_TMPDIR "/test_key.hex";

    /* env:// provider with 64 hex characters */
    setenv("CDB_TEST_KEY_ENV", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", 1);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crypto_resolve_key("env://CDB_TEST_KEY_ENV", 0, NULL, NULL, key));
    TEST_ASSERT_EQUAL_HEX8(0x01, key[0]);
    TEST_ASSERT_EQUAL_HEX8(0x23, key[1]);
    TEST_ASSERT_EQUAL_HEX8(0xef, key[31]);

    /* Missing env variable */
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_crypto_resolve_key("env://CDB_NONEXISTENT_KEY_VAR", 0, NULL, NULL, key));

    /* file:// provider in development mode */
    f = fopen(tmp_keyfile, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fprintf(f, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\n");
    fclose(f);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crypto_resolve_key("file://" CDB_TEST_TMPDIR "/test_key.hex", 0, NULL, NULL, key));
    TEST_ASSERT_EQUAL_HEX8(0x00, key[0]);
    TEST_ASSERT_EQUAL_HEX8(0x11, key[1]);
    TEST_ASSERT_EQUAL_HEX8(0xff, key[31]);

    /* file:// provider refused in production mode (spec 12.3) */
    TEST_ASSERT_EQUAL_INT(CDB_E_SECURITY, cdb_crypto_resolve_key("file://" CDB_TEST_TMPDIR "/test_key.hex", 1, NULL, NULL, key));

    /* Custom provider */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crypto_resolve_key("vault://secret/app/db", 1, custom_mock_provider, NULL, key));
    TEST_ASSERT_EQUAL_HEX8(0xEE, key[0]);

    remove(tmp_keyfile);
}

static void test_identities_and_signatures(void)
{
    cdb_identity id1, id2;
    uint8_t sig[CDB_SIGNATURE_SIZE];
    const char *msg = "Attributed transaction revision audit event";

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_identity_generate(&id1, 1001, 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_identity_generate(&id2, 1002, 1));

    /* Public keys are 33 bytes compressed */
    TEST_ASSERT_TRUE(id1.public_key[0] == 0x02 || id1.public_key[0] == 0x03);

    /* Sign message with id1 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_identity_sign(&id1, msg, strlen(msg), sig));

    /* Verify with id1's public key */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_identity_verify(id1.public_key, msg, strlen(msg), sig));

    /* Verify with wrong public key fails */
    TEST_ASSERT_EQUAL_INT(CDB_E_CORRUPT, cdb_identity_verify(id2.public_key, msg, strlen(msg), sig));

    /* Verify with tampered message fails */
    TEST_ASSERT_EQUAL_INT(CDB_E_CORRUPT, cdb_identity_verify(id1.public_key, "Tampered message", 16, sig));

    /* Verify with corrupted signature fails */
    sig[0] ^= 0x01;
    TEST_ASSERT_EQUAL_INT(CDB_E_CORRUPT, cdb_identity_verify(id1.public_key, msg, strlen(msg), sig));
}

static void test_crypto_wipe(void)
{
    uint8_t sensitive[64];
    size_t i;
    memset(sensitive, 0x5A, sizeof(sensitive));

    cdb_crypto_wipe(sensitive, sizeof(sensitive));
    for (i = 0; i < sizeof(sensitive); ++i)
        TEST_ASSERT_EQUAL_HEX8(0, sensitive[i]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_make_nonce);
    RUN_TEST(test_aead_aes_gcm_roundtrip);
    RUN_TEST(test_aead_chacha20_poly1305_roundtrip);
    RUN_TEST(test_sha256_and_hmac);
    RUN_TEST(test_subkey_derivation);
    RUN_TEST(test_key_wrapping);
    RUN_TEST(test_key_providers);
    RUN_TEST(test_identities_and_signatures);
    RUN_TEST(test_crypto_wipe);
    return UNITY_END();
}
