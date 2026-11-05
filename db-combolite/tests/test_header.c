#include <string.h>

#include "unity.h"
#include "cdb_crc32c.h"
#include "cdb_endian.h"
#include "cdb_faultdev.h"
#include "cdb_header.h"

#define CAP 8192

static uint8_t current[CAP], durable[CAP], log_data[CAP];
static cdb_fault_record log_records[32];
static cdb_faultdev dev;
static cdb_storage st;

static void init_device(void)
{
    cdb_faultdev_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.current = current;
    cfg.durable = durable;
    cfg.capacity = CAP;
    cfg.log = log_records;
    cfg.log_cap = 32;
    cfg.log_data = log_data;
    cfg.log_data_cap = CAP;
    cfg.geometry.program_unit = 1;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_faultdev_init(&dev, &cfg, &st));
}

static cdb_header sample_header(cdb_byte_order order)
{
    cdb_header h;
    unsigned i;
    memset(&h, 0, sizeof h);
    h.order = order;
    for (i = 0; i < 16; i++)
        h.db_id[i] = (uint8_t)(0xA0 + i);
    h.min_page_log2 = 9;
    h.default_page_log2 = 12;
    h.max_page_log2 = 16;
    h.checksum_alg = 1;
    h.aead_alg = 1;
    h.app_id = 0x474F5631u;
    h.creator_version = 0x000100u;
    h.writer_version = 0x000100u;
    h.created_us = UINT64_C(1793145600000000);
    h.page_seq_hwm = 1000;
    h.commit_hwm = 100;
    memcpy(h.key_ref, "vault://secret/cdb/test#v1", 26);
    h.companions[0].type_id = 7;
    h.companions[0].compat_class = 1;
    h.companions[0].state = 0x03;
    h.companions[0].watermark = 42;
    h.anchors[1].feature_index = 5;
    h.anchors[1].size_hint = 64;
    h.anchors[1].anchor = 9001;
    return h;
}

void setUp(void)
{
    init_device();
}

void tearDown(void) {}

static void encode_valid(cdb_header *h, uint8_t img[CDB_HEADER_SLOT_SIZE])
{
    h->epoch = 1;
    h->committed_seq = 1;
    h->ring_size = 2;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_header_encode(h, img));
}

/* Re-seal an image after a deliberate edit, so only the edit is under test. */
static void reseal(uint8_t img[CDB_HEADER_SLOT_SIZE], cdb_byte_order o)
{
    cdb_store_u32(img + 508, cdb_crc32c(img, 508), o);
}

/* ------------------------------------------------------- encode/decode */

static void test_roundtrip_both_orders(void)
{
    cdb_byte_order orders[2] = { CDB_ORDER_LITTLE, CDB_ORDER_BIG };
    int i;
    for (i = 0; i < 2; i++) {
        cdb_header h = sample_header(orders[i]), back;
        uint8_t img[CDB_HEADER_SLOT_SIZE];
        encode_valid(&h, img);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_header_decode(img, &back));
        TEST_ASSERT_EQUAL_MEMORY(&h.db_id, &back.db_id, 16);
        TEST_ASSERT_EQUAL_UINT64(h.page_seq_hwm, back.page_seq_hwm);
        TEST_ASSERT_EQUAL_UINT32(h.app_id, back.app_id);
        TEST_ASSERT_EQUAL_UINT64(42, back.companions[0].watermark);
        TEST_ASSERT_EQUAL_UINT64(9001, back.anchors[1].anchor);
        TEST_ASSERT_EQUAL_MEMORY(h.key_ref, back.key_ref, CDB_HEADER_KEYREF_LEN);
        TEST_ASSERT_EQUAL_INT(orders[i], back.order);
    }
}

static void test_byte_layout(void)
{
    cdb_header h = sample_header(CDB_ORDER_BIG);
    uint8_t img[CDB_HEADER_SLOT_SIZE];
    const uint8_t magic[8] = { 0x89, 'C', 'D', 'B', 0x0D, 0x0A, 0x1A, 0x0A };
    encode_valid(&h, img);
    TEST_ASSERT_EQUAL_MEMORY(magic, img, 8);
    TEST_ASSERT_EQUAL_HEX8('B', img[8]);
    TEST_ASSERT_EQUAL_HEX8(1, img[9]);
    TEST_ASSERT_EQUAL_HEX8(0x01, img[12]); /* 0x01020304 big-endian */
    TEST_ASSERT_EQUAL_HEX8(0x04, img[15]);
    TEST_ASSERT_EQUAL_MEMORY("vault://", img + 128, 8);
    TEST_ASSERT_EQUAL_HEX32(cdb_crc32c(img, 508), cdb_load_u32(img + 508, CDB_ORDER_BIG));
}

static void test_rejects_damage(void)
{
    cdb_header h = sample_header(CDB_ORDER_LITTLE), back;
    uint8_t img[CDB_HEADER_SLOT_SIZE];

    encode_valid(&h, img);
    img[200] ^= 0x10;
    TEST_ASSERT_EQUAL_INT(CDB_E_CORRUPT, cdb_header_decode(img, &back));

    encode_valid(&h, img);
    img[1] = 'X';
    TEST_ASSERT_EQUAL_INT(CDB_E_MALFORMED, cdb_header_decode(img, &back));

    encode_valid(&h, img);
    img[8] = 'Q';
    TEST_ASSERT_EQUAL_INT(CDB_E_MALFORMED, cdb_header_decode(img, &back));
}

static void test_rejects_bad_fields(void)
{
    cdb_header h = sample_header(CDB_ORDER_LITTLE), back;
    uint8_t img[CDB_HEADER_SLOT_SIZE];

    encode_valid(&h, img);
    img[9] = 2; /* a future epoch */
    reseal(img, CDB_ORDER_LITTLE);
    TEST_ASSERT_EQUAL_INT(CDB_E_UNSUPPORTED, cdb_header_decode(img, &back));

    encode_valid(&h, img);
    cdb_store_u32(img + 12, 0x04030201u, CDB_ORDER_LITTLE); /* wrong declared order */
    reseal(img, CDB_ORDER_LITTLE);
    TEST_ASSERT_EQUAL_INT(CDB_E_MALFORMED, cdb_header_decode(img, &back));

    encode_valid(&h, img);
    img[482] = 1; /* reserved byte (484..491 is the marks root) */
    reseal(img, CDB_ORDER_LITTLE);
    TEST_ASSERT_EQUAL_INT(CDB_E_MALFORMED, cdb_header_decode(img, &back));

    encode_valid(&h, img);
    img[11] = 0x80; /* unknown layout flag */
    reseal(img, CDB_ORDER_LITTLE);
    TEST_ASSERT_EQUAL_INT(CDB_E_MALFORMED, cdb_header_decode(img, &back));

    encode_valid(&h, img);
    img[128 + 100] = 'x'; /* text after the key reference's zero padding */
    reseal(img, CDB_ORDER_LITTLE);
    TEST_ASSERT_EQUAL_INT(CDB_E_MALFORMED, cdb_header_decode(img, &back));

    encode_valid(&h, img);
    img[72] = 13; /* minimum page size above the default */
    reseal(img, CDB_ORDER_LITTLE);
    TEST_ASSERT_EQUAL_INT(CDB_E_MALFORMED, cdb_header_decode(img, &back));
}

static void test_feature_classes(void)
{
    cdb_header h = sample_header(CDB_ORDER_LITTLE);
    int ro;
    /* A file without the store ID slot is refused (spec 14.2.7). */
    TEST_ASSERT_EQUAL_INT(CDB_E_UNSUPPORTED, cdb_header_check_features(&h, &ro));
    h.incompat_flags = CDB_FEATURE_IN_STORE_SLOT;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_header_check_features(&h, &ro));
    TEST_ASSERT_FALSE(ro);
    h.compat_flags = 0x1;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_header_check_features(&h, &ro));
    TEST_ASSERT_FALSE(ro);
    h.rocompat_flags = 0x2;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_header_check_features(&h, &ro));
    TEST_ASSERT_TRUE(ro);
    h.incompat_flags |= 0x4;
    TEST_ASSERT_EQUAL_INT(CDB_E_UNSUPPORTED, cdb_header_check_features(&h, &ro));
}

/* --------------------------------------------------- create/open/write */

static void test_create_and_open(void)
{
    cdb_header h = sample_header(CDB_ORDER_LITTLE);
    cdb_header_state hs, opened;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_header_create(&st, &h, &hs));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_header_open(&st, &opened));
    TEST_ASSERT_EQUAL_UINT64(1, opened.record.committed_seq);
    TEST_ASSERT_EQUAL_UINT(0, opened.record_slot); /* slot A is committed, B is its working copy */
    TEST_ASSERT_EQUAL_UINT(hs.working_slot, opened.working_slot);
    TEST_ASSERT_FALSE(opened.read_only);
}

static void test_empty_storage_is_not_found(void)
{
    cdb_header_state hs;
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_header_open(&st, &hs));
}

static void test_commit_and_checkpoint_rotate(void)
{
    cdb_header h = sample_header(CDB_ORDER_LITTLE);
    cdb_header_state hs, opened;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_header_create(&st, &h, &hs));

    h.anchor_root = 77;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_header_commit(&st, &hs, &h));   /* writes slot A */
    TEST_ASSERT_EQUAL_UINT(0, hs.record_slot);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_header_checkpoint(&st, &hs, 5000, 300)); /* writes slot B */
    TEST_ASSERT_EQUAL_UINT(0, hs.working_slot);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_header_open(&st, &opened));
    TEST_ASSERT_EQUAL_UINT64(2, opened.record.committed_seq);
    TEST_ASSERT_EQUAL_UINT64(77, opened.record.anchor_root);
    TEST_ASSERT_EQUAL_UINT64(5000, opened.record.page_seq_hwm);
    TEST_ASSERT_EQUAL_UINT64(300, opened.record.commit_hwm);
    TEST_ASSERT_EQUAL_UINT(0, opened.working_slot);
}

static void test_reservation_marks_never_drop(void)
{
    cdb_header h = sample_header(CDB_ORDER_LITTLE);
    cdb_header_state hs;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_header_create(&st, &h, &hs));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_header_checkpoint(&st, &hs, 9000, 900));
    h.page_seq_hwm = 10; /* a stale caller value must not lower the mark */
    h.commit_hwm = 1;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_header_commit(&st, &hs, &h));
    TEST_ASSERT_EQUAL_UINT64(9000, hs.record.page_seq_hwm);
    TEST_ASSERT_EQUAL_UINT64(900, hs.record.commit_hwm);
}

static void test_unknown_rocompat_opens_read_only(void)
{
    cdb_header h = sample_header(CDB_ORDER_LITTLE);
    cdb_header_state hs;
    h.rocompat_flags = UINT64_C(1) << 40;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_header_create(&st, &h, &hs));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_header_open(&st, &hs));
    TEST_ASSERT_TRUE(hs.read_only);
    TEST_ASSERT_EQUAL_INT(CDB_E_READ_ONLY, cdb_header_checkpoint(&st, &hs, 1, 1));
}

static void test_unknown_incompat_refuses(void)
{
    cdb_header h = sample_header(CDB_ORDER_LITTLE);
    cdb_header_state hs;
    h.incompat_flags = UINT64_C(1) << 63;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_header_create(&st, &h, &hs));
    TEST_ASSERT_TRUE(hs.record.incompat_flags & CDB_FEATURE_IN_STORE_SLOT); /* always set on create */
    TEST_ASSERT_EQUAL_INT(CDB_E_UNSUPPORTED, cdb_header_open(&st, &hs));
}

static void test_damaged_slot_falls_back(void)
{
    cdb_header h = sample_header(CDB_ORDER_LITTLE);
    cdb_header_state hs;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_header_create(&st, &h, &hs));
    h.anchor_root = 55;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_header_commit(&st, &hs, &h)); /* seq 2 in slot A */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_faultdev_flip_bit(&dev, 100, 3)); /* rot in slot A */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_header_open(&st, &hs));
    TEST_ASSERT_EQUAL_UINT64(1, hs.record.committed_seq); /* slot B: the previous commit */
    TEST_ASSERT_EQUAL_UINT(1, hs.record_slot);
    TEST_ASSERT_EQUAL_UINT(0, hs.working_slot); /* the damaged slot is rewritten next */
}

static void test_both_slots_damaged(void)
{
    cdb_header h = sample_header(CDB_ORDER_LITTLE);
    cdb_header_state hs;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_header_create(&st, &h, &hs));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_faultdev_flip_bit(&dev, 100, 0));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_faultdev_flip_bit(&dev, 612, 0));
    TEST_ASSERT_EQUAL_INT(CDB_E_CORRUPT, cdb_header_open(&st, &hs));
}

/* ------------------------------------------------------- crash sweep */

/* Workload: checkpoint (raises the page mark to 5000), then commit seq 2
 * with anchor root 77. Each header write is one program plus one sync. */
static cdb_status workload(cdb_header_state *hs, int *checkpoint_done, int *commit_done)
{
    cdb_header h = sample_header(CDB_ORDER_LITTLE);
    cdb_status s;
    *checkpoint_done = 0;
    *commit_done = 0;
    if ((s = cdb_header_checkpoint(&st, hs, 5000, 300)) != CDB_OK)
        return s;
    *checkpoint_done = 1;
    h.anchor_root = 77;
    if ((s = cdb_header_commit(&st, hs, &h)) != CDB_OK)
        return s;
    *commit_done = 1;
    return CDB_OK;
}

static void prepare(cdb_header_state *hs)
{
    cdb_header h = sample_header(CDB_ORDER_LITTLE);
    init_device();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_header_create(&st, &h, hs));
}

static void test_crash_sweep(void)
{
    const cdb_crash_mode modes[4] = { CDB_CRASH_DROP_UNSYNCED, CDB_CRASH_KEEP_PREFIX, CDB_CRASH_TORN,
                                      CDB_CRASH_REORDER };
    const size_t torn_points[5] = { 0, 1, 255, 508, 511 };
    cdb_header_state hs;
    uint64_t total_ops, op;
    int ckpt, commit;
    int m;

    /* Count the workload's mutating operations once. */
    prepare(&hs);
    {
        uint64_t before = dev.mutating_ops;
        TEST_ASSERT_EQUAL_INT(CDB_OK, workload(&hs, &ckpt, &commit));
        total_ops = dev.mutating_ops - before;
    }
    TEST_ASSERT_EQUAL_UINT64(4, total_ops);

    for (op = 1; op <= total_ops; op++) {
        for (m = 0; m < 4; m++) {
            size_t t, tears = modes[m] == CDB_CRASH_TORN ? 5 : 1;
            for (t = 0; t < tears; t++) {
                cdb_header_state after;
                prepare(&hs);
                /* KEEP_PREFIX keeps the struck write whole; TORN keeps part of it. */
                cdb_faultdev_arm(&dev, op, modes[m], modes[m] == CDB_CRASH_KEEP_PREFIX ? 1 : 0,
                                 torn_points[t]);
                TEST_ASSERT_NOT_EQUAL_INT(CDB_OK, workload(&hs, &ckpt, &commit));
                cdb_faultdev_reboot(&dev);

                /* Recovery must always find a valid header... */
                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_header_open(&st, &after));
                /* ...holding either the old or the new committed state, never a mix. */
                if (after.record.committed_seq == 2) {
                    TEST_ASSERT_EQUAL_UINT64(77, after.record.anchor_root);
                } else {
                    TEST_ASSERT_EQUAL_UINT64(1, after.record.committed_seq);
                    TEST_ASSERT_EQUAL_UINT64(0, after.record.anchor_root);
                }
                /* An acknowledged checkpoint's reservation is never forgotten (R1). */
                if (ckpt)
                    TEST_ASSERT_EQUAL_UINT64(5000, after.record.page_seq_hwm);
                /* An acknowledged commit always survives (C-R3). */
                if (commit)
                    TEST_ASSERT_EQUAL_UINT64(2, after.record.committed_seq);
                /* The file stays writable after recovery. */
                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_header_checkpoint(&st, &after, 6000, 400));
                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_header_open(&st, &after));
                TEST_ASSERT_EQUAL_UINT64(6000, after.record.page_seq_hwm);
            }
        }
    }
}

/* The scenario that motivated rotating writes: a reservation saved by a
 * checkpoint must survive a commit that is torn by a crash. */
static void test_torn_commit_keeps_checkpoint_reservation(void)
{
    cdb_header h = sample_header(CDB_ORDER_LITTLE);
    cdb_header_state hs, after;
    prepare(&hs);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_header_checkpoint(&st, &hs, 7777, 300));
    cdb_faultdev_arm(&dev, 1, CDB_CRASH_TORN, 0, 200);
    TEST_ASSERT_EQUAL_INT(CDB_E_IO, cdb_header_commit(&st, &hs, &h));
    cdb_faultdev_reboot(&dev);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_header_open(&st, &after));
    TEST_ASSERT_EQUAL_UINT64(1, after.record.committed_seq);
    TEST_ASSERT_EQUAL_UINT64(7777, after.record.page_seq_hwm);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_roundtrip_both_orders);
    RUN_TEST(test_byte_layout);
    RUN_TEST(test_rejects_damage);
    RUN_TEST(test_rejects_bad_fields);
    RUN_TEST(test_feature_classes);
    RUN_TEST(test_create_and_open);
    RUN_TEST(test_empty_storage_is_not_found);
    RUN_TEST(test_commit_and_checkpoint_rotate);
    RUN_TEST(test_reservation_marks_never_drop);
    RUN_TEST(test_unknown_rocompat_opens_read_only);
    RUN_TEST(test_unknown_incompat_refuses);
    RUN_TEST(test_damaged_slot_falls_back);
    RUN_TEST(test_both_slots_damaged);
    RUN_TEST(test_crash_sweep);
    RUN_TEST(test_torn_commit_keeps_checkpoint_reservation);
    return UNITY_END();
}
