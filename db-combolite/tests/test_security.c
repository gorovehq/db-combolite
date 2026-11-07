#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "cdb_db.h"
#include "cdb_faultenv.h"
#include "combolite/cdb_crypto.h"

#define FCAP (640u * 1024u)
#define PAGE_LOG2 9u
#define PAGE (1u << PAGE_LOG2)
#define NODES 16u
#define STORE 1u

static uint8_t env_arena[CDB_FAULTENV_MAX_FILES * 3u * FCAP];
static cdb_fault_record env_records[CDB_FAULTENV_MAX_FILES * 4096u];
static cdb_faultenv fe;
static cdb_env env;

static uint8_t arena[NODES * 2u * PAGE + 2u * PAGE];
static cdb_pager_node nodes[NODES];
static cdb_free_extent free_list[8192];
static uint8_t record_buf[1024];
static uint8_t cat_work[16384];
static cdb_index_entry cat_entries[512];
static uint8_t cat_page[PAGE];
static uint8_t page[PAGE];
static cdb_db_config cfg;
static cdb_db db;

#define KV_PAGES 128u

static cdb_status kv_apply(void *ctx, cdb_pager *p, const cdb_delta *d)
{
    cdb_page_header t;
    uint64_t lid;
    cdb_status s;
    (void)ctx;
    TEST_ASSERT_TRUE(d->entry_id < KV_PAGES);
    if (d->op == CDB_OP_DELETE) {
        s = cdb_pager_free(p, d->entry_id);
        return s == CDB_E_NOT_FOUND ? CDB_OK : s;
    }
    while (p->next_lid < KV_PAGES)
        if ((s = cdb_pager_alloc(p, &lid)) != CDB_OK)
            return s;
    memset(&t, 0, sizeof t);
    t.type = CDB_PAGE_TYPE_DATA_TEST;
    t.store_id = d->store_id;
    t.domain_id = d->store_id >= 42u ? d->store_id : 0u; /* stores from 42 are enclaves */
    return cdb_pager_write(p, d->entry_id, &t, d->data, d->len);
}

static cdb_header settings(void)
{
    cdb_header h;
    unsigned i;
    memset(&h, 0, sizeof h);
    h.order = CDB_ORDER_LITTLE;
    h.layout_flags = CDB_LAYOUT_ADDR64;
    h.default_page_log2 = (uint8_t)PAGE_LOG2;
    for (i = 0; i < 16; i++)
        h.db_id[i] = (uint8_t)(0x30 + i);
    return h;
}

static void init_env(void)
{
    cdb_faultenv_config c;
    c.arena = env_arena;
    c.file_capacity = FCAP;
    c.records = env_records;
    c.records_per_file = 4096;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_faultenv_init(&fe, &c, &env));
}

void setUp(void)
{
    init_env();
    memset(&cfg, 0, sizeof cfg);
    cfg.pager.arena = arena;
    cfg.pager.arena_size = sizeof arena;
    cfg.pager.nodes = nodes;
    cfg.pager.node_cap = NODES;
    cfg.pager.free_list = free_list;
    cfg.pager.free_cap = sizeof free_list / sizeof free_list[0];
    cfg.record_buf = record_buf;
    cfg.record_buf_size = sizeof record_buf;
    cfg.apply = kv_apply;
    cfg.catalog_work.bytes = cat_work;
    cfg.catalog_work.bytes_size = sizeof cat_work;
    cfg.catalog_work.entries = cat_entries;
    cfg.catalog_work.entries_cap = 512;
    cfg.catalog_page = cat_page;
    cfg.durability = CDB_DURABILITY_FULL;

    /* Set up test env vars */
    setenv("CDB_TEST_MK", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", 1);
    setenv("CDB_TEST_MK_NEW", "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210", 1);
    setenv("CDB_TEST_ENC", "11223344556677889900aabbccddeeff11223344556677889900aabbccddeeff", 1);
    setenv("CDB_TEST_ENC_V2", "22334455667788990011bbccddeeff0022334455667788990011bbccddeeff00", 1);
}

void tearDown(void) {}

static cdb_status custom_provider_test(const char *uri, uint8_t key_out[CDB_CRYPTO_KEY_SIZE], void *user_data)
{
    (void)user_data;
    if (strcmp(uri, "custom://secret/key1") == 0) {
        memset(key_out, 0x42, CDB_CRYPTO_KEY_SIZE);
        return CDB_OK;
    }
    return CDB_E_NOT_FOUND;
}

/* ------------------------------------------------------------ Key Providers */

static void test_key_provider_env_and_custom(void)
{
    cdb_header h = settings();
    cfg.master_key_uri = "env://CDB_TEST_MK";
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db, &env, "sec_env", &cfg, &h));
    TEST_ASSERT_TRUE(db.has_master_key);
    TEST_ASSERT_TRUE(db.has_integrity_key);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));

    /* Custom provider */
    cfg.master_key_uri = "custom://secret/key1";
    cfg.key_provider = custom_provider_test;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db, &env, "sec_cust", &cfg, &h));
    TEST_ASSERT_TRUE(db.has_master_key);
    TEST_ASSERT_EQUAL_INT(0x42, db.master_key[0]);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_key_provider_file_refusal(void)
{
    FILE *f;
    uint8_t k[32];
    cdb_header h = settings();

    memset(k, 0xAA, sizeof k);
    f = fopen("/tmp/cdb_test_key.bin", "wb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_INT(1, fwrite(k, sizeof k, 1, f));
    fclose(f);

    cfg.master_key_uri = "file:///tmp/cdb_test_key.bin";

    /* Dev mode: succeeds */
    cfg.is_production = 0;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db, &env, "sec_file_dev", &cfg, &h));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));

    /* Production mode: refuses */
    cfg.is_production = 1;
    TEST_ASSERT_EQUAL_INT(CDB_E_SECURITY, cdb_db_open(&db, &env, "sec_file_prod", &cfg, &h));

    remove("/tmp/cdb_test_key.bin");
}

/* ------------------------------------------------------------ Governance Mode */

static void test_tamper_evident_governance_mode(void)
{
    cdb_header h = settings();
    h.layout_flags |= CDB_LAYOUT_GOVERNANCE;

    /* Governance mode without master key fails */
    cfg.master_key_uri = NULL;
    TEST_ASSERT_EQUAL_INT(CDB_E_SECURITY, cdb_db_open(&db, &env, "sec_gov", &cfg, &h));

    /* With master key, it opens and enables integrity key */
    cfg.master_key_uri = "env://CDB_TEST_MK";
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db, &env, "sec_gov", &cfg, &h));
    TEST_ASSERT_TRUE(db.has_master_key);
    TEST_ASSERT_TRUE(db.has_integrity_key);

    /* Write some records in governance mode */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&db, STORE, 1, CDB_OP_REPLACE, "tamper_proof", 12));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));

    /* Reopen with correct master key */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db, &env, "sec_gov", &cfg, NULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));

    /* Reopen with wrong master key fails to authenticate */
    setenv("CDB_TEST_MK_WRONG", "0000000000000000000000000000000000000000000000000000000000000000", 1);
    cfg.master_key_uri = "env://CDB_TEST_MK_WRONG";
    TEST_ASSERT_EQUAL_INT(CDB_E_CORRUPT, cdb_db_open(&db, &env, "sec_gov", &cfg, NULL));
}

/* ------------------------------------------------------------ Enclave Encryption */

static void test_enclave_create_and_roundtrip(void)
{
    cdb_header h = settings();
    cfg.master_key_uri = "env://CDB_TEST_MK";
    cfg.aead_alg = CDB_AEAD_AES_256_GCM;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db, &env, "sec_enc", &cfg, &h));

    /* Create enclave 42 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_create_enclave(&db, 42, "env://CDB_TEST_ENC"));

    /* Duplicate create fails */
    TEST_ASSERT_EQUAL_INT(CDB_E_EXISTS, cdb_db_create_enclave(&db, 42, "env://CDB_TEST_ENC"));

    /* Write encrypted data to enclave 42 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&db, 42, 1, CDB_OP_REPLACE, "enclave_payload", 15));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));

    /* Read back within session */
    {
        cdb_page_header ph;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_read(&db.pager, 1, page, &ph));
        TEST_ASSERT_EQUAL_INT(15, ph.payload_len);
        TEST_ASSERT_EQUAL_MEMORY("enclave_payload", page + cdb_page_payload_offset(db.pager.fmt), 15);
    }

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));

    /* Reopen database */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db, &env, "sec_enc", &cfg, NULL));

    /* Before opening enclave, reading page fails with CDB_E_SECURITY */
    {
        cdb_page_header ph;
        TEST_ASSERT_EQUAL_INT(CDB_E_SECURITY, cdb_pager_read(&db.pager, 1, page, &ph));
    }

    /* Open enclave 42 with key */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open_enclave(&db, 42, "env://CDB_TEST_ENC"));

    /* Now reading page succeeds and is decrypted */
    {
        cdb_page_header ph;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_read(&db.pager, 1, page, &ph));
        TEST_ASSERT_EQUAL_INT(15, ph.payload_len);
        TEST_ASSERT_EQUAL_MEMORY("enclave_payload", page + cdb_page_payload_offset(db.pager.fmt), 15);
    }

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* ------------------------------------------------------------ Key Rotation */

static void test_master_key_rotation(void)
{
    cdb_header h = settings();
    cfg.master_key_uri = "env://CDB_TEST_MK";
    cfg.aead_alg = CDB_AEAD_AES_256_GCM;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db, &env, "sec_rot_mk", &cfg, &h));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_create_enclave(&db, 42, "env://CDB_TEST_ENC"));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&db, 42, 1, CDB_OP_REPLACE, "persistent_data", 15));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));

    /* Rotate master key to CDB_TEST_MK_NEW */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_rotate_master_key(&db, "env://CDB_TEST_MK_NEW"));

    /* Verify enclave data remains readable under rotated master key */
    {
        cdb_page_header ph;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_read(&db.pager, 1, page, &ph));
        TEST_ASSERT_EQUAL_INT(15, ph.payload_len);
        TEST_ASSERT_EQUAL_MEMORY("persistent_data", page + cdb_page_payload_offset(db.pager.fmt), 15);
    }

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_enclave_key_rotation(void)
{
    cdb_header h = settings();
    cfg.master_key_uri = "env://CDB_TEST_MK";
    cfg.aead_alg = CDB_AEAD_AES_256_GCM;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db, &env, "sec_rot_enc", &cfg, &h));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_create_enclave(&db, 42, "env://CDB_TEST_ENC"));

    /* Initial write with version 1 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&db, 42, 1, CDB_OP_REPLACE, "v1_payload", 10));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));

    /* Rotate enclave key to version 2 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_rotate_enclave_key(&db, 42, "env://CDB_TEST_ENC_V2"));
    TEST_ASSERT_EQUAL_INT(2, db.enclaves[0].key_version);

    /* Write new data with version 2 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&db, 42, 2, CDB_OP_REPLACE, "v2_payload", 10));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));

    /* Read back v2 */
    {
        cdb_page_header ph;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_read(&db.pager, 2, page, &ph));
        TEST_ASSERT_EQUAL_INT(10, ph.payload_len);
        TEST_ASSERT_EQUAL_MEMORY("v2_payload", page + cdb_page_payload_offset(db.pager.fmt), 10);
    }

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_rotation_timer_policies(void)
{
    cdb_header h = settings();
    cfg.master_key_uri = "env://CDB_TEST_MK";
    cfg.rotation_interval_days = 90;

    /* Policy REFUSE_OPEN: refuses when expired */
    cfg.rotation_policy = CDB_ROTATION_REFUSE_OPEN;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db, &env, "sec_pol_open", &cfg, &h));
    db.rotation_expired = 1;
    TEST_ASSERT_EQUAL_INT(CDB_E_SECURITY, cdb_db_check_key_rotation(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));

    /* Policy REFUSE_WRITES: opens fine, but refuses writes */
    cfg.rotation_policy = CDB_ROTATION_REFUSE_WRITES;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db, &env, "sec_pol_write", &cfg, &h));
    db.rotation_expired = 1;
    TEST_ASSERT_EQUAL_INT(CDB_E_SECURITY, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));

    /* Policy WARN: opens and allows writes even when expired */
    cfg.rotation_policy = CDB_ROTATION_WARN;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db, &env, "sec_pol_warn", &cfg, &h));
    db.rotation_expired = 1;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&db, STORE, 1, CDB_OP_REPLACE, "ok", 2));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* ------------------------------------------------------------ Identities & RBAC */

static void test_rbac_authorization(void)
{
    cdb_header h = settings();
    uint8_t dummy_pubkey[33];
    memset(dummy_pubkey, 0x02, sizeof dummy_pubkey);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db, &env, "sec_rbac", &cfg, &h));

    /* Register principal 101 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_register_principal(&db, 101, 1, dummy_pubkey));

    /* Grant read on store 42 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_grant_role(&db, 101, 1, 42, CDB_PERM_READ));

    /* Check perms */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_check_perm(&db, 101, 42, CDB_PERM_READ));
    TEST_ASSERT_EQUAL_INT(CDB_E_SECURITY, cdb_db_check_perm(&db, 101, 42, CDB_PERM_WRITE));
    TEST_ASSERT_EQUAL_INT(CDB_E_SECURITY, cdb_db_check_perm(&db, 101, 99, CDB_PERM_READ));
    TEST_ASSERT_EQUAL_INT(CDB_E_SECURITY, cdb_db_check_perm(&db, 202, 42, CDB_PERM_READ));

    /* Register admin principal 1 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_register_principal(&db, 1, 1, dummy_pubkey));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_grant_role(&db, 1, 1, 0, CDB_PERM_ADMIN));

    /* Admin has all permissions on all scopes */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_check_perm(&db, 1, 42, CDB_PERM_READ));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_check_perm(&db, 1, 42, CDB_PERM_WRITE));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_check_perm(&db, 1, 99, CDB_PERM_EXEC));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_commit_identity_signing(void)
{
    cdb_identity id;
    cdb_header h = settings();

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_identity_generate(&id, 101, 1));

    cfg.identity = &id;
    cfg.signing_policy = CDB_SIGN_AUDIT_ONLY;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db, &env, "sec_sign", &cfg, &h));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&db, STORE, 1, CDB_OP_REPLACE, "signed_tx", 9));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* ------------------------------------------------------------ Main */

/* ------------------------------------------------------------ Session salts (12.1, 14.2.8) */

/* Opens "name" with the master key and makes enclave 42 usable: created
 * with the file, opened on a later session. */
static void enc_open(const char *name, const cdb_header *h)
{
    cfg.master_key_uri = "env://CDB_TEST_MK";
    cfg.aead_alg = CDB_AEAD_AES_256_GCM;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db, &env, name, &cfg, h));
    if (h != NULL)
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_create_enclave(&db, 42, "env://CDB_TEST_ENC"));
    else
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open_enclave(&db, 42, "env://CDB_TEST_ENC"));
}

static cdb_status put_text(uint64_t store, uint64_t entry, const char *text)
{
    cdb_status s;
    if ((s = cdb_tx_begin(&db, CDB_DURABILITY_FULL)) != CDB_OK)
        return s;
    if ((s = cdb_tx_put(&db, store, entry, CDB_OP_REPLACE, text, (uint32_t)strlen(text))) != CDB_OK)
        return s;
    return cdb_tx_commit(&db);
}

static void expect_text(uint64_t lid, const char *text)
{
    cdb_page_header ph;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_read(&db.pager, lid, page, &ph));
    TEST_ASSERT_EQUAL_UINT32(strlen(text), ph.payload_len);
    TEST_ASSERT_EQUAL_MEMORY(text, page + cdb_page_payload_offset(db.pager.fmt), strlen(text));
}

static uint16_t salt_index_of(uint64_t lid)
{
    cdb_page_header ph;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_peek(&db.pager, lid, page, &ph, NULL));
    return ph.salt_index;
}

/* The salt table as the archive holds it: every encrypted page names an
 * entry, and every entry's salt is one this test drew. */
static void check_salts(void)
{
    int done = 0;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_scrub(&db, UINT64_MAX, &done));
    TEST_ASSERT_EQUAL_INT(1, done);
}

/* Each session that opens with a master key draws its own salt, which its
 * journal header carries; an archive page names its session's salt by its
 * index in the salt table, which later sessions load (spec 12.1, 14.2.8). */
static void test_session_salts(void)
{
    cdb_header h = settings();
    uint8_t first[16], zero[16];

    memset(zero, 0, sizeof zero);
    enc_open("salts", &h);
    TEST_ASSERT_TRUE(db.has_salt);
    TEST_ASSERT_TRUE(memcmp(db.session_salt, zero, 16) != 0);
    TEST_ASSERT_EQUAL_MEMORY(db.session_salt, db.journal.hdr.salt, 16);
    memcpy(first, db.session_salt, 16);
    TEST_ASSERT_EQUAL_INT(CDB_OK, put_text(42, 1, "first"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, put_text(STORE, 3, "public"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&db));
    TEST_ASSERT_EQUAL_UINT(1, db.salt_count);
    TEST_ASSERT_EQUAL_UINT16(1, db.salts[0].index);
    TEST_ASSERT_EQUAL_MEMORY(first, db.salts[0].salt, 16);
    TEST_ASSERT_EQUAL_UINT64(db.pager.hs.record.committed_seq, db.salts[0].last_commit);
    TEST_ASSERT_EQUAL_UINT16(1, salt_index_of(1));
    TEST_ASSERT_EQUAL_UINT16(0, salt_index_of(3)); /* a public page names none */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));

    enc_open("salts", NULL);
    TEST_ASSERT_TRUE(memcmp(first, db.session_salt, 16) != 0);
    TEST_ASSERT_EQUAL_MEMORY(db.session_salt, db.journal.hdr.salt, 16);
    TEST_ASSERT_EQUAL_UINT(1, db.salt_count);
    expect_text(1, "first"); /* under the first session's salt */
    TEST_ASSERT_EQUAL_INT(CDB_OK, put_text(42, 2, "second"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&db));
    TEST_ASSERT_EQUAL_UINT(2, db.salt_count);
    TEST_ASSERT_EQUAL_UINT16(2, salt_index_of(2));
    TEST_ASSERT_EQUAL_UINT16(1, salt_index_of(1));
    check_salts();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));

    enc_open("salts", NULL);
    expect_text(1, "first");
    expect_text(2, "second");
    expect_text(3, "public");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* Without a random source, a database with a master key can't draw a salt,
 * so it can't be written (read-only sessions don't exist yet); without a
 * master key it needs none. Encryption needs a master key. */
static void test_salt_needs_entropy_and_master_key(void)
{
    cdb_header h = settings();
    cdb_env_ops ops = *env.ops;
    cdb_env plain = env;

    ops.random = NULL;
    plain.ops = &ops;
    cfg.master_key_uri = "env://CDB_TEST_MK";
    TEST_ASSERT_EQUAL_INT(CDB_E_READ_ONLY, cdb_db_open(&db, &plain, "noentropy", &cfg, &h));

    cfg.master_key_uri = NULL;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db, &plain, "noentropy", &cfg, &h));
    TEST_ASSERT_FALSE(db.has_salt);
    TEST_ASSERT_EQUAL_INT(CDB_E_SECURITY, cdb_db_create_enclave(&db, 42, "env://CDB_TEST_ENC"));
    TEST_ASSERT_EQUAL_INT(CDB_E_SECURITY, cdb_db_open_enclave(&db, 42, "env://CDB_TEST_ENC"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* A scrub pass finds which salts pages use; a salt no page used, last used
 * before the pass began, leaves the table at the next archive commit, and
 * its index is reused (spec 14.2.8). */
static void test_scrub_prunes_salts(void)
{
    cdb_header h = settings();
    int done = 0;
    uint64_t pass_start;

    enc_open("prune", &h);
    TEST_ASSERT_EQUAL_INT(CDB_OK, put_text(42, 1, "s1"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
    enc_open("prune", NULL);
    TEST_ASSERT_EQUAL_INT(CDB_OK, put_text(42, 2, "s2"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));

    enc_open("prune", NULL);
    TEST_ASSERT_EQUAL_UINT(2, db.salt_count);
    TEST_ASSERT_EQUAL_INT(CDB_OK, put_text(42, 1, "s3 one"));
    /* Not reconciled yet: page 1's new version hides its committed one,
     * still under salt 1, so a pass now marks nothing. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_scrub(&db, UINT64_MAX, &done));
    TEST_ASSERT_EQUAL_INT(1, done);
    TEST_ASSERT_EQUAL_UINT64(0, db.salts[0].prune_before);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&db));
    TEST_ASSERT_EQUAL_UINT(3, db.salt_count);

    /* A pass a page at a time: salt 2 is still in use. */
    pass_start = db.pager.hs.record.committed_seq;
    do
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_scrub(&db, 1, &done));
    while (!done);
    TEST_ASSERT_EQUAL_UINT64(pass_start, db.salts[0].prune_before);
    TEST_ASSERT_EQUAL_UINT64(0, db.salts[1].prune_before);
    TEST_ASSERT_EQUAL_UINT64(0, db.salts[2].prune_before); /* this session's */
    TEST_ASSERT_EQUAL_UINT(3, db.salt_count); /* not before a commit */
    TEST_ASSERT_EQUAL_INT(CDB_OK, put_text(STORE, 3, "public"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&db));
    TEST_ASSERT_EQUAL_UINT(2, db.salt_count);
    TEST_ASSERT_EQUAL_UINT16(2, db.salts[0].index);
    TEST_ASSERT_EQUAL_UINT16(3, db.salts[1].index);

    /* Rewriting salt 2's page makes it a candidate on the next pass; a page
     * written under it after the pass began would have saved it. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, put_text(42, 2, "s3 two"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&db));
    check_salts();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db)); /* the close's commit prunes */

    enc_open("prune", NULL);
    TEST_ASSERT_EQUAL_UINT(1, db.salt_count);
    TEST_ASSERT_EQUAL_UINT16(3, db.salts[0].index);
    TEST_ASSERT_EQUAL_INT(CDB_OK, put_text(42, 4, "s4"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&db));
    TEST_ASSERT_EQUAL_UINT16(1, salt_index_of(4)); /* the lowest free index */
    expect_text(1, "s3 one");
    expect_text(2, "s3 two");
    check_salts();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* Run before the crash clock is armed. Session 1 writes public entries
 * 1..30 and encrypted 31..50; session 2 rewrites the encrypted ones and
 * deletes the public ones, so session 1's salt is unused and the low pages
 * are free for compaction. */
static void salt_sweep_setup(void)
{
    cdb_header h = settings();
    char v[16];
    unsigned i;

    enc_open("sweep", &h);
    for (i = 1; i <= 50; i++) {
        snprintf(v, sizeof v, "a%u", i);
        TEST_ASSERT_EQUAL_INT(CDB_OK, put_text(i <= 30 ? STORE : 42, i, v));
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
    enc_open("sweep", NULL);
    for (i = 31; i <= 50; i++) {
        snprintf(v, sizeof v, "b%u", i);
        TEST_ASSERT_EQUAL_INT(CDB_OK, put_text(42, i, v));
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    for (i = 1; i <= 30; i++)
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&db, STORE, i, CDB_OP_DELETE, NULL, 0));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* The swept work. Session 3 finds salt 1 unused, prunes it in a commit, then
 * compaction re-encrypts moved pages under its own salt, adding it to the
 * table in the commit that writes them. Its journal holds only public
 * records, so recovery needs no enclave key. */
static cdb_status salt_sweep_work(uint64_t *moved)
{
    uint64_t reclaimed;
    int done = 0;
    cdb_status s;

    cfg.master_key_uri = "env://CDB_TEST_MK";
    cfg.aead_alg = CDB_AEAD_AES_256_GCM;
    if ((s = cdb_db_open(&db, &env, "sweep", &cfg, NULL)) != CDB_OK
        || (s = cdb_db_open_enclave(&db, 42, "env://CDB_TEST_ENC")) != CDB_OK
        || (s = cdb_db_scrub(&db, UINT64_MAX, &done)) != CDB_OK
        || (s = put_text(STORE, 70, "pruned")) != CDB_OK
        || (s = cdb_db_reconcile(&db)) != CDB_OK
        || (s = cdb_db_compact(&db, UINT64_MAX, moved, &reclaimed)) != CDB_OK)
        return s;
    return cdb_db_close(&db);
}

/* Whatever the crash left: every encrypted page decrypts under a salt the
 * table holds, the data is session 2's, and the database still works. */
static void salt_sweep_check(void)
{
    cdb_page_header ph;
    cdb_status s;
    char v[16];
    unsigned i;

    enc_open("sweep", NULL);
    check_salts();
    for (i = 31; i <= 50; i++) {
        snprintf(v, sizeof v, "b%u", i);
        expect_text(i, v);
    }
    s = cdb_pager_read(&db.pager, 70, page, &ph);
    TEST_ASSERT_TRUE(s == CDB_E_NOT_FOUND || s == CDB_OK);
    if (s == CDB_OK)
        expect_text(70, "pruned");
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_pager_read(&db.pager, 1, page, &ph));
    TEST_ASSERT_EQUAL_INT(CDB_OK, put_text(42, 60, "later"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
    enc_open("sweep", NULL);
    expect_text(60, "later");
    check_salts();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* Crash sweep over the salt table's write sequences (spec 14.2.8): the
 * prune commit and the commits that add a salt. */
static void test_salt_crash_sweep(void)
{
    const cdb_crash_mode modes[4] = { CDB_CRASH_DROP_UNSYNCED, CDB_CRASH_KEEP_PREFIX, CDB_CRASH_TORN,
                                      CDB_CRASH_REORDER };
    const size_t tears[3] = { 0, 30, 300 };
    uint64_t total, op, base, moved = 0;
    unsigned runs = 0;
    int m;

    salt_sweep_setup();
    base = fe.clock.ops;
    TEST_ASSERT_EQUAL_INT(CDB_OK, salt_sweep_work(&moved));
    total = fe.clock.ops - base;
    TEST_ASSERT_TRUE(moved > 0);
    {
        char msg[64];
        snprintf(msg, sizeof msg, "crash points swept: %u", (unsigned)total);
        TEST_MESSAGE(msg);
    }
    cdb_faultenv_reboot(&fe);
    enc_open("sweep", NULL);
    /* Session 1's salt was pruned and its index went to session 3's, which
     * compaction wrote pages under after session 2's. */
    TEST_ASSERT_EQUAL_UINT(2, db.salt_count);
    TEST_ASSERT_EQUAL_UINT16(1, db.salts[0].index);
    TEST_ASSERT_EQUAL_UINT16(2, db.salts[1].index);
    TEST_ASSERT_TRUE(db.salts[0].last_commit > db.salts[1].last_commit);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
    salt_sweep_check();

    for (op = 1; op <= total; op++) {
        for (m = 0; m < 4; m++) {
            size_t t, nt = modes[m] == CDB_CRASH_TORN ? 3 : 1;
            for (t = 0; t < nt; t++) {
                init_env();
                salt_sweep_setup();
                cdb_faultenv_arm(&fe, op, modes[m], modes[m] == CDB_CRASH_KEEP_PREFIX ? 1 : 0, tears[t]);
                TEST_ASSERT_NOT_EQUAL_INT(CDB_OK, salt_sweep_work(&moved));
                cdb_faultenv_reboot(&fe);
                salt_sweep_check();
                runs++;
            }
        }
    }
    TEST_ASSERT_EQUAL_UINT((unsigned)(total * 6u), runs);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_key_provider_env_and_custom);
    RUN_TEST(test_key_provider_file_refusal);
    RUN_TEST(test_tamper_evident_governance_mode);
    RUN_TEST(test_enclave_create_and_roundtrip);
    RUN_TEST(test_master_key_rotation);
    RUN_TEST(test_enclave_key_rotation);
    RUN_TEST(test_rotation_timer_policies);
    RUN_TEST(test_rbac_authorization);
    RUN_TEST(test_commit_identity_signing);
    RUN_TEST(test_session_salts);
    RUN_TEST(test_salt_needs_entropy_and_master_key);
    RUN_TEST(test_scrub_prunes_salts);
    RUN_TEST(test_salt_crash_sweep);
    return UNITY_END();
}
