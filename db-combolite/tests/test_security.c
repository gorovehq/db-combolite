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
    t.domain_id = d->store_id;
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
    return UNITY_END();
}
