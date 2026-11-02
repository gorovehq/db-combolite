#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "combolite/cdb_crypto.h"
#include "combolite/cdb_blob.h"
#include "combolite/cdb_value.h"
#include "cdb_db.h"
#include "cdb_faultdev.h"
#include "cdb_faultenv.h"
#include "cdb_journal.h"
#include "cdb_overflow.h"

#define FCAP (1024u * 1024u)
#define PAGE_LOG2 9u /* 512-byte pages */
#define PAGE (1u << PAGE_LOG2)
#define NODES 32u
#define TEST_STORE_ID 77u

static uint8_t env_arena[CDB_FAULTENV_MAX_FILES * 3u * FCAP];
static cdb_fault_record env_records[CDB_FAULTENV_MAX_FILES * 4096u];
static cdb_faultenv fe;
static cdb_env env;

static uint8_t arena[NODES * 2u * PAGE + 2u * PAGE];
static cdb_pager_node nodes[NODES];
static cdb_free_extent free_list[8192];
static uint8_t record_buf[2048];
static uint8_t cat_work[16384];
static cdb_index_entry cat_entries[512];
static uint8_t cat_page[PAGE];
static uint8_t doc_buffers[8u * PAGE];
static uint8_t doc_stage[4096];
static uint8_t page_buf[PAGE];
static uint8_t scratch_buf[PAGE];
static cdb_db_config cfg;
static cdb_db db;

static void init_env(void)
{
    cdb_faultenv_config c;
    c.arena = env_arena;
    c.file_capacity = FCAP;
    c.records = env_records;
    c.records_per_file = 4096;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_faultenv_init(&fe, &c, &env));
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
        h.db_id[i] = (uint8_t)(0x40 + i);
    return h;
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
    cfg.catalog_work.bytes = cat_work;
    cfg.catalog_work.bytes_size = sizeof cat_work;
    cfg.catalog_work.entries = cat_entries;
    cfg.catalog_work.entries_cap = 512;
    cfg.catalog_page = cat_page;
    cfg.doc_buffers = doc_buffers;
    cfg.doc_stage = doc_stage;
    cfg.doc_stage_size = sizeof doc_stage;
    cfg.durability = CDB_DURABILITY_FULL;

    setenv("CDB_TEST_MK", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", 1);
    setenv("CDB_TEST_ENC", "11223344556677889900aabbccddeeff11223344556677889900aabbccddeeff", 1);
}

void tearDown(void) {}

/* ------------------------------------------------------------ Crypto Tests */

static void test_crypto_hash_incremental_sha256(void)
{
    cdb_crypto_hash_ctx ctx;
    uint8_t out[CDB_CRYPTO_HASH_SIZE];
    uint8_t oneshot[CDB_CRYPTO_HASH_SIZE];
    const char *msg = "abcdefghijklmnopqrstuvwxyz0123456789";
    size_t len = strlen(msg);

    /* NIST vector: empty string */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crypto_hash_init(&ctx));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crypto_hash_final(&ctx, out));
    cdb_crypto_hash_free(&ctx);
    /* e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855 */
    TEST_ASSERT_EQUAL_HEX8(0xe3, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0xb0, out[1]);
    TEST_ASSERT_EQUAL_HEX8(0xc4, out[2]);
    TEST_ASSERT_EQUAL_HEX8(0x55, out[31]);

    /* Compare incremental chunks vs one-shot */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crypto_sha256(msg, len, oneshot));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crypto_hash_init(&ctx));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crypto_hash_update(&ctx, msg, 5));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crypto_hash_update(&ctx, msg + 5, 12));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crypto_hash_update(&ctx, msg + 17, len - 17));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crypto_hash_final(&ctx, out));
    cdb_crypto_hash_free(&ctx);

    TEST_ASSERT_EQUAL_HEX8_ARRAY(oneshot, out, CDB_CRYPTO_HASH_SIZE);
}

static void test_crypto_hash_incremental_hmac_sha256(void)
{
    cdb_crypto_hash_ctx ctx;
    uint8_t key[CDB_CRYPTO_KEY_SIZE];
    uint8_t out[CDB_CRYPTO_HASH_SIZE];
    uint8_t oneshot[CDB_CRYPTO_HASH_SIZE];
    const char *msg = "Streaming HMAC-SHA-256 for enclave data integrity verification";
    size_t len = strlen(msg);

    memset(key, 0x5c, sizeof(key));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crypto_hmac_sha256(key, msg, len, oneshot));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crypto_hash_init_hmac(&ctx, key));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crypto_hash_update(&ctx, msg, 10));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crypto_hash_update(&ctx, msg + 10, 20));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crypto_hash_update(&ctx, msg + 30, len - 30));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crypto_hash_final(&ctx, out));
    cdb_crypto_hash_free(&ctx);

    TEST_ASSERT_EQUAL_HEX8_ARRAY(oneshot, out, CDB_CRYPTO_HASH_SIZE);
}

/* ------------------------------------------------------------ Streaming Blob Core */

static void test_blob_empty(void)
{
    cdb_header h = settings();
    cdb_blob_writer writer;
    cdb_blob_reader reader;
    cdb_blob_ref ref;
    size_t bytes_read = 999;
    uint8_t dummy[16];

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db, &env, "test_b_empty", &cfg, &h));

    /* Write 0-byte blob */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_blob_write_begin(&db.pager, TEST_STORE_ID, scratch_buf, NULL, &writer));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_blob_write_end(&writer, &ref));

    TEST_ASSERT_EQUAL_UINT64(0, ref.total_len);
    TEST_ASSERT_EQUAL_UINT64(0, ref.first_page);
    TEST_ASSERT_EQUAL_UINT64(0, ref.page_count);

    /* Read back 0-byte blob */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_blob_read_begin(&db.pager, TEST_STORE_ID, &ref, page_buf, 1, NULL, &reader));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_blob_read_chunk(&reader, dummy, sizeof(dummy), &bytes_read));
    TEST_ASSERT_EQUAL_UINT64(0, bytes_read);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_blob_read_end(&reader));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_blob_multipage_roundtrip(void)
{
    cdb_header h = settings();
    cdb_blob_writer writer;
    cdb_blob_reader reader;
    cdb_blob_ref ref;
    static uint8_t test_payload[16384];
    static uint8_t read_back[16384];
    size_t i;
    size_t offset;

    /* Fill payload deterministically */
    for (i = 0; i < sizeof(test_payload); ++i)
        test_payload[i] = (uint8_t)((i * 37u + 13u) & 0xFFu);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db, &env, "test_b_multi", &cfg, &h));

    /* Write using uneven chunk sizes */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_blob_write_begin(&db.pager, TEST_STORE_ID, scratch_buf, NULL, &writer));

    const size_t chunk_sizes[] = { 1, 13, 256, 777, 1024, 2048, 513, 4096, 311 };
    size_t chunk_idx = 0;
    offset = 0;
    while (offset < sizeof(test_payload)) {
        size_t csz = chunk_sizes[chunk_idx % (sizeof(chunk_sizes) / sizeof(chunk_sizes[0]))];
        chunk_idx++;
        if (offset + csz > sizeof(test_payload))
            csz = sizeof(test_payload) - offset;

        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_blob_write_chunk(&writer, test_payload + offset, csz));
        offset += csz;
    }

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_blob_write_end(&writer, &ref));
    TEST_ASSERT_EQUAL_UINT64(sizeof(test_payload), ref.total_len);
    TEST_ASSERT_TRUE(ref.page_count > 1);
    TEST_ASSERT_NOT_EQUAL(0, ref.first_page);

    /* Read back with different uneven buffer sizes and verify hash */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_blob_read_begin(&db.pager, TEST_STORE_ID, &ref, page_buf, 1, NULL, &reader));

    const size_t read_sizes[] = { 3, 17, 100, 500, 1024, 4000 };
    size_t r_idx = 0;
    offset = 0;
    while (offset < sizeof(test_payload)) {
        size_t rsz = read_sizes[r_idx % (sizeof(read_sizes) / sizeof(read_sizes[0]))];
        r_idx++;
        size_t got = 0;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_blob_read_chunk(&reader, read_back + offset, rsz, &got));
        TEST_ASSERT_TRUE(got > 0);
        offset += got;
    }

    /* Next read chunk should return got == 0 (EOF) and verify hash */
    size_t got_eof = 1;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_blob_read_chunk(&reader, scratch_buf, sizeof(scratch_buf), &got_eof));
    TEST_ASSERT_EQUAL_UINT64(0, got_eof);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_blob_read_end(&reader));

    /* Check that every single byte matched */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(test_payload, read_back, sizeof(test_payload));

    /* Free the blob */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_blob_free(&db.pager, TEST_STORE_ID, &ref, page_buf));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_blob_tamper_detection(void)
{
    cdb_header h = settings();
    cdb_blob_writer writer;
    cdb_blob_reader reader;
    cdb_blob_ref ref;
    static uint8_t test_payload[4096];
    static uint8_t read_back[4096];
    size_t i;

    for (i = 0; i < sizeof(test_payload); ++i)
        test_payload[i] = (uint8_t)(i & 0xFFu);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db, &env, "test_b_tamper", &cfg, &h));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_blob_write_begin(&db.pager, TEST_STORE_ID, scratch_buf, NULL, &writer));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_blob_write_chunk(&writer, test_payload, sizeof(test_payload)));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_blob_write_end(&writer, &ref));

    /* 1. Tamper with hash */
    cdb_blob_ref tampered_ref = ref;
    tampered_ref.hash[5] ^= 0x55;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_blob_read_begin(&db.pager, TEST_STORE_ID, &tampered_ref, page_buf, 1, NULL, &reader));
    size_t got = 0;
    cdb_status s = cdb_blob_read_chunk(&reader, read_back, sizeof(read_back), &got);
    if (s == CDB_OK) {
        /* If full buffer was read, the EOF check triggers on this call or the next */
        s = cdb_blob_read_chunk(&reader, scratch_buf, 1, &got);
    }
    TEST_ASSERT_EQUAL_INT(CDB_E_CORRUPT, s);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_blob_read_end(&reader));

    /* 2. Tamper with a payload page directly */
    cdb_page_header ph;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_read(&db.pager, ref.first_page, page_buf, &ph));
    /* Modify payload byte */
    page_buf[cdb_page_header_size(db.pager.fmt) + db.pager.addr_width + 10] ^= 0xAA;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_write(&db.pager, ref.first_page, &ph,
                                                 page_buf + cdb_page_header_size(db.pager.fmt),
                                                 ph.payload_len));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_blob_read_begin(&db.pager, TEST_STORE_ID, &ref, page_buf, 1, NULL, &reader));
    s = cdb_blob_read_chunk(&reader, read_back, sizeof(read_back), &got);
    if (s == CDB_OK) {
        s = cdb_blob_read_chunk(&reader, scratch_buf, 1, &got);
    }
    TEST_ASSERT_EQUAL_INT(CDB_E_CORRUPT, s);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_blob_read_end(&reader));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_blob_abort_reclamation(void)
{
    cdb_header h = settings();
    cdb_blob_writer writer;
    uint8_t dummy[2048];
    memset(dummy, 0xBB, sizeof(dummy));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db, &env, "test_b_abort", &cfg, &h));

    uint64_t initial_next_lid = db.pager.next_lid;

    /* Start writing and allocate several pages */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_blob_write_begin(&db.pager, TEST_STORE_ID, scratch_buf, NULL, &writer));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_blob_write_chunk(&writer, dummy, sizeof(dummy)));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_blob_write_chunk(&writer, dummy, sizeof(dummy)));
    TEST_ASSERT_TRUE(writer.page_count > 2);

    /* Abort writing: should reclaim all pages allocated so far */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_blob_write_abort(&writer));
    TEST_ASSERT_EQUAL_INT(0, writer.is_open);

    /* Subsequent allocation should successfully reuse the freed logical pages */
    uint64_t realloc_lid = 0;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_alloc(&db.pager, &realloc_lid));
    TEST_ASSERT_TRUE(realloc_lid >= initial_next_lid);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* ------------------------------------------------------------ Value Encoding */

static void test_blob_value_encoding(void)
{
    uint8_t buf[256];
    cdb_value_writer w;
    cdb_value val;
    cdb_blob_ref original_ref;

    memset(&original_ref, 0, sizeof(original_ref));
    memset(original_ref.hash, 0x7E, sizeof(original_ref.hash));
    original_ref.total_len = 1048576;
    original_ref.first_page = 42;
    original_ref.page_count = 2100;
    original_ref.flags = 0x01; /* Bit 0: external file */

    /* 1. Root blob value */
    cdb_value_writer_init(&w, buf, sizeof(buf), CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_blob_ref(&w, &original_ref));
    size_t enc_len = cdb_value_writer_len(&w);
    TEST_ASSERT_TRUE(enc_len > 0);

    /* Validate canonical form */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_validate(buf, enc_len, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT, 0));

    /* Decode value */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_read(buf, enc_len, CDB_ORDER_LITTLE, &val));
    TEST_ASSERT_EQUAL_INT(CDB_TYPE_BLOB_REF, val.type);
    TEST_ASSERT_EQUAL_UINT32(original_ref.flags, val.as.blob.flags);
    TEST_ASSERT_EQUAL_UINT64(original_ref.total_len, val.as.blob.total_len);
    TEST_ASSERT_EQUAL_UINT64(original_ref.first_page, val.as.blob.first_page);
    TEST_ASSERT_EQUAL_UINT64(original_ref.page_count, val.as.blob.page_count);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(original_ref.hash, val.as.blob.hash, CDB_CRYPTO_HASH_SIZE);

    /* 2. Blob reference nested in object */
    cdb_value_writer_init(&w, buf, sizeof(buf), CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_begin_object(&w));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_field(&w, 10));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_int(&w, 12345));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_field(&w, 20));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_blob_ref(&w, &original_ref));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_end_object(&w));

    enc_len = cdb_value_writer_len(&w);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_validate(buf, enc_len, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT, 0));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_read(buf, enc_len, CDB_ORDER_LITTLE, &val));
    TEST_ASSERT_EQUAL_INT(CDB_TYPE_OBJECT, val.type);

    cdb_value field20;
    int found = 0;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_object_get(&val, CDB_ORDER_LITTLE, 20, &field20, &found));
    TEST_ASSERT_EQUAL_INT(1, found);
    TEST_ASSERT_EQUAL_INT(CDB_TYPE_BLOB_REF, field20.type);
    TEST_ASSERT_EQUAL_UINT64(original_ref.total_len, field20.as.blob.total_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(original_ref.hash, field20.as.blob.hash, CDB_CRYPTO_HASH_SIZE);
}

/* ------------------------------------------------------------ Multipart Journal */

static void test_blob_multipart_journal(void)
{
    cdb_header h = settings();
    cdb_journal j;
    cdb_storage st;
    cdb_journal_header jh;
    cdb_record r_in, r_out;
    uint64_t next_off;
    uint8_t j_buf[1024];
    static uint8_t cur[8192], dur[8192], logd[8192];
    static cdb_fault_record recs[64];
    cdb_faultdev dev;
    cdb_faultdev_config fc;
    const char *payload_chunk = "streaming blob journal frame fragment";

    memset(&fc, 0, sizeof(fc));
    fc.current = cur;
    fc.durable = dur;
    fc.capacity = sizeof(cur);
    fc.log = recs;
    fc.log_cap = sizeof(recs) / sizeof(recs[0]);
    fc.log_data = logd;
    fc.log_data_cap = sizeof(logd);
    fc.geometry.program_unit = 1;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_faultdev_init(&dev, &fc, &st));

    memset(&jh, 0, sizeof(jh));
    jh.type_id = CDB_COMPANION_MAIN_JOURNAL;
    memcpy(jh.db_id, h.db_id, sizeof(jh.db_id));
    jh.unsealed_seq = 1;
    jh.first_seq = 1;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_journal_create(&j, &st, CDB_ORDER_LITTLE, &jh));

    /* Write record with CDB_RECORD_FLAG_CONTINUED set */
    memset(&r_in, 0, sizeof(r_in));
    r_in.kind = CDB_RECORD_DATA;
    r_in.flags = CDB_RECORD_FLAG_CONTINUED;
    r_in.store_id = TEST_STORE_ID;
    r_in.entry_id = 1;
    r_in.payload = (const uint8_t *)payload_chunk;
    r_in.payload_len = (uint32_t)strlen(payload_chunk);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_journal_append(&j, &r_in, j_buf, sizeof(j_buf)));

    /* Read back and verify continued flag is preserved */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_journal_read(&j, CDB_JOURNAL_HEADER_SIZE, j_buf, sizeof(j_buf), &r_out, &next_off));
    TEST_ASSERT_TRUE(r_out.flags & CDB_RECORD_FLAG_CONTINUED);
    TEST_ASSERT_EQUAL_UINT32(strlen(payload_chunk), r_out.payload_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(payload_chunk, r_out.payload, r_out.payload_len);
}

/* ------------------------------------------------------------ Database Integration */

static void test_db_blob_high_level(void)
{
    cdb_header h = settings();
    cdb_blob_writer writer;
    cdb_blob_reader reader;
    cdb_blob_ref ref;
    const char *data = "High-level database blob integration test with full validation.";
    size_t len = strlen(data);
    char out_buf[128];
    size_t bytes_read = 0;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db, &env, "test_b_hl", &cfg, &h));

    /* Write using high level wrapper */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_blob_write_begin(&db, TEST_STORE_ID, scratch_buf, &writer));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_blob_write_chunk(&writer, data, len));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_blob_write_end(&writer, &ref));

    TEST_ASSERT_EQUAL_UINT64(len, ref.total_len);

    /* Read using high level wrapper */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_blob_read_begin(&db, TEST_STORE_ID, &ref, page_buf, 1, &reader));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_blob_read_chunk(&reader, out_buf, sizeof(out_buf), &bytes_read));
    TEST_ASSERT_EQUAL_UINT64(len, bytes_read);
    out_buf[bytes_read] = '\0';
    TEST_ASSERT_EQUAL_STRING(data, out_buf);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_blob_read_end(&reader));

    /* Free blob */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_blob_free(&db, TEST_STORE_ID, &ref, page_buf));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_db_blob_enclave(void)
{
    cdb_header h = settings();
    cfg.master_key_uri = "env://CDB_TEST_MK";
    cfg.aead_alg = CDB_AEAD_AES_256_GCM;
    uint64_t enclave_id = 42;

    cdb_blob_writer writer;
    cdb_blob_reader reader;
    cdb_blob_ref ref;
    const char *secret_doc = "Enclave-protected secret blob payload for zero-trust streaming.";
    size_t len = strlen(secret_doc);
    char out_buf[128];
    size_t bytes_read = 0;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db, &env, "test_b_enc", &cfg, &h));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_create_enclave(&db, enclave_id, "env://CDB_TEST_ENC"));

    /* Write blob to enclave store */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_blob_write_begin(&db, enclave_id, scratch_buf, &writer));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_blob_write_chunk(&writer, secret_doc, len));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_blob_write_end(&writer, &ref));

    /* Verify enclave flag is recorded */
    TEST_ASSERT_TRUE(ref.flags & 0x02); /* Bit 1: enclave */

    /* Verify that hash is HMAC-SHA-256 under enclave key rather than plain SHA-256 */
    uint8_t plain_sha[CDB_CRYPTO_HASH_SIZE];
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crypto_sha256(secret_doc, len, plain_sha));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(plain_sha, ref.hash, CDB_CRYPTO_HASH_SIZE));

    /* Read back with enclave verification */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_blob_read_begin(&db, enclave_id, &ref, page_buf, 1, &reader));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_blob_read_chunk(&reader, out_buf, sizeof(out_buf), &bytes_read));
    TEST_ASSERT_EQUAL_UINT64(len, bytes_read);
    out_buf[bytes_read] = '\0';
    TEST_ASSERT_EQUAL_STRING(secret_doc, out_buf);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_blob_read_end(&reader));

    /* Reading under non-enclave store ID should fail due to store isolation */
    TEST_ASSERT_EQUAL_INT(CDB_E_CORRUPT, cdb_db_blob_read_begin(&db, TEST_STORE_ID, &ref, page_buf, 1, &reader));

    /* Reading under enclave_id without the enclave key should fail HMAC hash verification */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_blob_read_begin(&db.pager, enclave_id, &ref, page_buf, 1, NULL, &reader));
    cdb_status s = cdb_blob_read_chunk(&reader, out_buf, sizeof(out_buf), &bytes_read);
    if (s == CDB_OK) {
        s = cdb_blob_read_chunk(&reader, scratch_buf, 1, &bytes_read);
    }
    TEST_ASSERT_EQUAL_INT(CDB_E_CORRUPT, s);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_blob_read_end(&reader));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_crypto_hash_incremental_sha256);
    RUN_TEST(test_crypto_hash_incremental_hmac_sha256);
    RUN_TEST(test_blob_empty);
    RUN_TEST(test_blob_multipage_roundtrip);
    RUN_TEST(test_blob_tamper_detection);
    RUN_TEST(test_blob_abort_reclamation);
    RUN_TEST(test_blob_value_encoding);
    RUN_TEST(test_blob_multipart_journal);
    RUN_TEST(test_db_blob_high_level);
    RUN_TEST(test_db_blob_enclave);
    return UNITY_END();
}
