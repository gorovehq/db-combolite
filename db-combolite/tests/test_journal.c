#include <string.h>

#include "unity.h"
#include "combolite/cdb_crypto.h"
#include "cdb_faultdev.h"
#include "cdb_endian.h"
#include "cdb_journal.h"

#define CAP 8192u

static uint8_t current[CAP], durable[CAP], log_data[CAP], buf[512];
static cdb_fault_record log_records[64];
static cdb_faultdev dev;
static cdb_storage st;
static cdb_journal j;

static cdb_journal_header jh(void)
{
    cdb_journal_header h;
    memset(&h, 0, sizeof h);
    h.type_id = CDB_COMPANION_MAIN_JOURNAL;
    memset(h.db_id, 0x5A, sizeof h.db_id);
    h.unsealed_seq = 7;
    h.first_seq = 1;
    return h;
}

void setUp(void)
{
    cdb_faultdev_config c;
    cdb_journal_header h = jh();
    memset(&c, 0, sizeof c);
    c.current = current;
    c.durable = durable;
    c.capacity = CAP;
    c.log = log_records;
    c.log_cap = 64;
    c.log_data = log_data;
    c.log_data_cap = CAP;
    c.geometry.program_unit = 1;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_faultdev_init(&dev, &c, &st));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_journal_create(&j, &st, CDB_ORDER_LITTLE, &h));
}

void tearDown(void) {}

static void append_data(uint64_t tx, const char *text)
{
    cdb_record r;
    memset(&r, 0, sizeof r);
    r.kind = CDB_RECORD_DATA;
    r.tx_id = tx;
    r.store_id = 1;
    r.entry_id = 9;
    r.payload = (const uint8_t *)text;
    r.payload_len = (uint32_t)strlen(text);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_journal_append(&j, &r, buf, sizeof buf));
}

static void test_header_roundtrip(void)
{
    cdb_journal back;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_journal_open(&back, &st, CDB_ORDER_LITTLE));
    TEST_ASSERT_EQUAL_UINT64(7, back.hdr.unsealed_seq);
    TEST_ASSERT_EQUAL_HEX8(0x5A, back.hdr.db_id[0]);
    TEST_ASSERT_EQUAL_INT(CDB_E_CORRUPT, cdb_journal_open(&back, &st, CDB_ORDER_BIG));
}

static void test_records_roundtrip(void)
{
    cdb_record r;
    uint64_t next;
    append_data(3, "hello");
    append_data(3, "world!");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_journal_read(&j, CDB_JOURNAL_HEADER_SIZE, buf, sizeof buf, &r, &next));
    TEST_ASSERT_EQUAL_UINT64(1, r.seq);
    TEST_ASSERT_EQUAL_UINT64(3, r.tx_id);
    TEST_ASSERT_EQUAL_MEMORY("hello", r.payload, 5);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_journal_read(&j, next, buf, sizeof buf, &r, &next));
    TEST_ASSERT_EQUAL_UINT64(2, r.seq);
    TEST_ASSERT_EQUAL_MEMORY("world!", r.payload, 6);
    TEST_ASSERT_EQUAL_INT(CDB_E_TRUNCATED, cdb_journal_read(&j, next, buf, sizeof buf, &r, &next));
}

static void test_torn_record_ends_journal(void)
{
    cdb_record r;
    uint64_t next;
    append_data(1, "kept");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_sync(&st));
    cdb_faultdev_arm(&dev, 1, CDB_CRASH_TORN, 0, 30);
    {
        cdb_record t;
        memset(&t, 0, sizeof t);
        t.kind = CDB_RECORD_DATA;
        t.payload = (const uint8_t *)"torn away";
        t.payload_len = 9;
        TEST_ASSERT_EQUAL_INT(CDB_E_IO, cdb_journal_append(&j, &t, buf, sizeof buf));
    }
    cdb_faultdev_reboot(&dev);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_journal_read(&j, CDB_JOURNAL_HEADER_SIZE, buf, sizeof buf, &r, &next));
    TEST_ASSERT_NOT_EQUAL_INT(CDB_OK, cdb_journal_read(&j, next, buf, sizeof buf, &r, &next));
}

static void test_commit_payload(void)
{
    uint8_t p[CDB_COMMIT_PAYLOAD_SIZE];
    cdb_commit_info in, out;
    cdb_record r;
    memset(&in, 0, sizeof in);
    in.commit_number = 42;
    in.delta_count = 3;
    cdb_commit_encode(&in, p, CDB_ORDER_BIG);
    memset(&r, 0, sizeof r);
    r.kind = CDB_RECORD_COMMIT;
    r.payload = p;
    r.payload_len = sizeof p;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_commit_decode(&r, &out, CDB_ORDER_BIG));
    TEST_ASSERT_EQUAL_UINT64(42, out.commit_number);
    TEST_ASSERT_EQUAL_UINT32(3, out.delta_count);
    r.kind = CDB_RECORD_DATA;
    TEST_ASSERT_EQUAL_INT(CDB_E_MALFORMED, cdb_commit_decode(&r, &out, CDB_ORDER_BIG));
}

static void test_encrypted_records_roundtrip(void)
{
    cdb_record r1, r2, read;
    uint8_t key[32];
    uint64_t next;
    const char *p1 = "Encrypted delta payload data 1";
    const char *p2 = "Encrypted delta payload data 2";

    memset(key, 0x33, sizeof(key));

    memset(&r1, 0, sizeof(r1));
    r1.kind = CDB_RECORD_DATA;
    r1.flags = CDB_RECORD_FLAG_ENCRYPTED;
    r1.payload = (const uint8_t *)p1;
    r1.payload_len = (uint32_t)strlen(p1) + 1;
    r1.store_id = 10;
    r1.entry_id = 101;
    r1.domain_id = 42;

    memset(&r2, 0, sizeof(r2));
    r2.kind = CDB_RECORD_DATA;
    r2.flags = CDB_RECORD_FLAG_ENCRYPTED;
    r2.payload = (const uint8_t *)p2;
    r2.payload_len = (uint32_t)strlen(p2) + 1;
    r2.store_id = 300;
    r2.entry_id = 102;
    r2.domain_id = 42;

    /* Append using AES-256-GCM */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_journal_append_crypto(&j, &r1, buf, sizeof(buf),
                                                           CDB_AEAD_AES_256_GCM, key));
    /* Append using ChaCha20-Poly1305 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_journal_append_crypto(&j, &r2, buf, sizeof(buf),
                                                           CDB_AEAD_CHACHA20_POLY1305, key));

    /* Read back r1 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_journal_read_crypto(&j, CDB_JOURNAL_HEADER_SIZE, buf, sizeof(buf),
                                                         &read, &next,
                                                         CDB_AEAD_AES_256_GCM, key));
    TEST_ASSERT_TRUE(read.flags & CDB_RECORD_FLAG_ENCRYPTED);
    TEST_ASSERT_EQUAL_STRING(p1, (const char *)read.payload);
    TEST_ASSERT_EQUAL_UINT32(strlen(p1) + 1, read.payload_len);
    TEST_ASSERT_EQUAL_UINT64(10, read.store_id);
    TEST_ASSERT_EQUAL_UINT64(101, read.entry_id);
    TEST_ASSERT_EQUAL_UINT64(42, read.domain_id);

    /* The header shows only the domain (spec 14.4.2); the payload length
     * counts the two varints (1 and 1 byte). */
    {
        uint8_t raw[CDB_RECORD_HEADER_SIZE];
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_read(&j.st, CDB_JOURNAL_HEADER_SIZE, raw, sizeof raw));
        TEST_ASSERT_EQUAL_UINT64(42, cdb_load_u64(raw + 24, j.order));
        TEST_ASSERT_EQUAL_UINT64(0, cdb_load_u64(raw + 32, j.order));
        TEST_ASSERT_EQUAL_UINT32(strlen(p1) + 1 + 2, cdb_load_u32(raw + 4, j.order));
    }

    /* Read back r2 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_journal_read_crypto(&j, next, buf, sizeof(buf),
                                                         &read, &next,
                                                         CDB_AEAD_CHACHA20_POLY1305, key));
    TEST_ASSERT_TRUE(read.flags & CDB_RECORD_FLAG_ENCRYPTED);
    TEST_ASSERT_EQUAL_STRING(p2, (const char *)read.payload);
    TEST_ASSERT_EQUAL_UINT64(300, read.store_id); /* a two-byte varint */
    TEST_ASSERT_EQUAL_UINT64(102, read.entry_id);

    /* Read with wrong key fails verification */
    key[0] ^= 0x01;
    TEST_ASSERT_EQUAL_INT(CDB_E_CORRUPT, cdb_journal_read_crypto(&j, CDB_JOURNAL_HEADER_SIZE, buf, sizeof(buf),
                                                                &read, &next,
                                                                CDB_AEAD_AES_256_GCM, key));
    key[0] ^= 0x01;

    /* An encrypted record must name its domain. */
    r1.domain_id = 0;
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_journal_append_crypto(&j, &r1, buf, sizeof(buf),
                                                                      CDB_AEAD_AES_256_GCM, key));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_header_roundtrip);
    RUN_TEST(test_records_roundtrip);
    RUN_TEST(test_torn_record_ends_journal);
    RUN_TEST(test_commit_payload);
    RUN_TEST(test_encrypted_records_roundtrip);
    return UNITY_END();
}
