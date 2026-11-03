/* Unit tests for CMBQL CRUD command parity across all C99 engine operations.
 * Tests both Declarative SQL syntax and Functional/Piped Flux syntax.
 */
#define _POSIX_C_SOURCE 200809L

#include "unity.h"
#include "cdb_compiler.h"
#include "cdb_proc.h"
#include "cdb_db.h"
#include "cdb_faultenv.h"
#include "combolite/cdb_crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FCAP (640u * 1024u)
#define PAGE_LOG2 9u
#define PAGE (1u << PAGE_LOG2)
#define NODES 16u

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
static cdb_db_config cfg;
static cdb_db db;

/* Model buffers for stores */
static uint8_t doc_bufs[16384];
static uint8_t doc_stage[16384];
static uint8_t idx_stage[16384];

static cdb_header default_header(void)
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

static void db_test_open(void)
{
    cdb_header h;
    cdb_status s;

    setenv("CDB_TEST_MK", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", 1);
    setenv("CDB_TEST_ENC", "11223344556677889900aabbccddeeff11223344556677889900aabbccddeeff", 1);

    init_env();
    h = default_header();

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
    cfg.catalog_work.entries_cap = sizeof cat_entries / sizeof cat_entries[0];
    cfg.catalog_page = cat_page;
    cfg.doc_buffers = doc_bufs;
    cfg.doc_stage = doc_stage;
    cfg.doc_stage_size = sizeof doc_stage;
    cfg.idx_stage = idx_stage;
    cfg.idx_stage_size = sizeof idx_stage;
    cfg.master_key_uri = "env://CDB_TEST_MK";
    cfg.aead_alg = CDB_AEAD_AES_256_GCM;
    cfg.durability = CDB_DURABILITY_FULL;

    s = cdb_db_open(&db, &env, "test_crud_parity", &cfg, &h);
    TEST_ASSERT_EQUAL_INT(CDB_OK, s);
}

static void db_test_close(void)
{
    cdb_db_close(&db);
}

void setUp(void) {}
void tearDown(void) {}

/* Helper to compile and assert success */
static void assert_compiles(const char *script)
{
    uint8_t bc[4096];
    size_t bc_len = 0;
    cdb_compile_error err;
    memset(&err, 0, sizeof(err));

    cdb_status s = cdb_compile_proc(script, strlen(script), NULL, bc, sizeof(bc), &bc_len, &err);
    TEST_ASSERT_EQUAL_MESSAGE(CDB_OK, s, err.msg);
    TEST_ASSERT_TRUE(bc_len > CDB_PROC_HEADER_SIZE);
}

/* Helper to compile and run bytecode against active test db */
static cdb_status run_script(cdb_db *test_db, const char *script, cdb_value *out_val)
{
    uint8_t bc[4096];
    size_t bc_len = 0;
    cdb_compile_error err;
    memset(&err, 0, sizeof(err));

    cdb_status s = cdb_compile_proc(script, strlen(script), NULL, bc, sizeof(bc), &bc_len, &err);
    if (s != CDB_OK) return s;

    cdb_proc_vm vm;
    cdb_proc_vm_init(&vm, test_db, CDB_ORDER_LITTLE, NULL, 0, 1000000);
    return cdb_proc_exec(&vm, bc, bc_len, out_val);
}

/* 1. Declarative SQL compilation tests covering all 37 engine operations */
static void test_compile_declarative_sql_parity(void)
{
    /* Enclaves & Keys */
    assert_compiles("CREATE ENCLAVE 1 KEY \"vault://keys/enclave1\";");
    assert_compiles("OPEN ENCLAVE 1 KEY \"vault://keys/enclave1\";");
    assert_compiles("ROTATE MASTER KEY \"vault://keys/master2\";");
    assert_compiles("ROTATE ENCLAVE 1 KEY \"vault://keys/enclave2\";");
    assert_compiles("CHECK ROTATION;");

    /* RBAC */
    assert_compiles("REGISTER PRINCIPAL 101 TENANT 1 PUBKEY \"0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798\";");
    assert_compiles("GRANT ROLE 1 TO 101 SCOPE 500 PERMS 15;");
    assert_compiles("CHECK PERM 101 SCOPE 500 PERMS 1;");

    /* DDL & Catalog */
    assert_compiles("CREATE STORE \"docs\" MODEL document;");
    assert_compiles("CREATE STORE \"users\" MODEL table;");
    assert_compiles("CREATE STORE \"metrics\" MODEL timeseries;");
    assert_compiles("CREATE STORE \"emb\" MODEL vector;");
    assert_compiles("DROP STORE \"temp\";");
    assert_compiles("ADD SCHEMA \"docs\" \"{\\\"type\\\":\\\"object\\\"}\";");

    /* Triggers */
    assert_compiles("CREATE TRIGGER trg_audit ON 101 AFTER INSERT EXECUTE \"audit_fn\" MAX DEPTH 8;");
    assert_compiles("DROP TRIGGER trg_audit;");

    /* Document Mutations */
    assert_compiles("INSERT INTO \"docs\" (\"{\\\"title\\\":\\\"Dune\\\"}\", \"Dune summary\");");
    assert_compiles("UPDATE \"docs\" SET \"{\\\"title\\\":\\\"Dune Part 2\\\"}\" WHERE id = 1;");
    assert_compiles("PATCH \"docs\" 1 WITH \"{\\\"rating\\\":5}\";");
    assert_compiles("REPLACE \"docs\" 1 WITH \"{\\\"title\\\":\\\"Dune Messiah\\\"}\";");
    assert_compiles("DELETE FROM \"docs\" WHERE id = 1;");

    /* Table Mutations */
    assert_compiles("INSERT INTO TABLE \"users\" (\"{\\\"name\\\":\\\"Alice\\\"}\");");
    assert_compiles("UPDATE TABLE \"users\" SET \"{\\\"name\\\":\\\"Bob\\\"}\" WHERE id = 10;");
    assert_compiles("DELETE FROM TABLE \"users\" WHERE id = 10;");

    /* Link Mutations */
    assert_compiles("CREATE LINK TYPE \"authored\" CARDINALITY 1:N ENFORCED;");
    assert_compiles("LINK 1 FROM 10:100 TO 20:200 WEIGHT 2.5 TIME 5000;");
    assert_compiles("REINFORCE LINK 1 FROM 10:100 TO 20:200 WEIGHT 1.5;");
    assert_compiles("UNLINK 1 FROM 10:100 TO 20:200;");

    /* Time Series Mutations */
    assert_compiles("CREATE SERIES \"metrics\" \"cpu\";");
    assert_compiles("APPEND INTO TIMESERIES \"metrics\" SERIES 1 TIME 1000 VALUE 42.5;");
    assert_compiles("PRUNE TIMESERIES \"metrics\" SERIES 1 BEFORE 500;");
    assert_compiles("SET RETENTION TIMESERIES \"metrics\" SERIES 1 AGE 3600;");
    assert_compiles("APPLY RETENTION TIMESERIES \"metrics\" SERIES 1 TIME 5000;");
    assert_compiles("DOWNSAMPLE TIMESERIES SOURCE 1:1 INTO 1:2 STEP 60 FROM 0 TO 1000;");

    /* Vector Mutations */
    assert_compiles("INSERT INTO VECTORS \"emb\" (\"\\x00\\x01\\x02\");");
    assert_compiles("DELETE FROM VECTORS \"emb\" WHERE id = 1;");

    /* Blob & Maintenance */
    assert_compiles("FREE BLOB 1 PAGE 5 COUNT 1 LEN 1024;");
    assert_compiles("COMPACT MAX 100;");
    assert_compiles("RECLAIM;");
    assert_compiles("RECONCILE;");

    /* CRDT Replication */
    assert_compiles("SET PEER 2;");
    assert_compiles("SYNC IMPORT \"slice_data\";");
}

/* 2. Functional & Piped Flux compilation tests covering all 37 engine operations */
static void test_compile_flux_crud_parity(void)
{
    /* Enclaves & Keys */
    assert_compiles("create_enclave(id: 1, key: \"vault://keys/enclave1\");");
    assert_compiles("open_enclave(id: 1, key: \"vault://keys/enclave1\");");
    assert_compiles("rotate(target: \"master\", key: \"vault://keys/master2\");");
    assert_compiles("rotate(target: \"enclave\", id: 1, key: \"vault://keys/enclave2\");");
    assert_compiles("check_rotation();");

    /* RBAC */
    assert_compiles("register_principal(id: 101, tenant: 1, pubkey: \"0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798\");");
    assert_compiles("grant_role(role: 1, to: 101, scope: 500, perms: 15);");
    assert_compiles("check_perm(principal: 101, scope: 500, perms: 1);");

    /* DDL & Catalog */
    assert_compiles("create_store(name: \"docs\", model: \"document\");");
    assert_compiles("create_store(name: \"users\", model: \"table\");");
    assert_compiles("drop_store(\"temp\");");
    assert_compiles("add_schema(store: \"docs\", schema: \"{\\\"type\\\":\\\"object\\\"}\");");

    /* Triggers */
    assert_compiles("create_trigger(name: \"trg_audit\", on: 101, events: 1, execute: \"audit_fn\", max_depth: 8);");
    assert_compiles("drop_trigger(\"trg_audit\");");

    /* Document Mutations */
    assert_compiles("insert(store: \"docs\", data: \"{\\\"title\\\":\\\"Dune\\\"}\", summary: \"Dune summary\");");
    assert_compiles("patch(store: \"docs\", entry: 1, patch: \"{\\\"rating\\\":5}\");");
    assert_compiles("replace(store: \"docs\", entry: 1, data: \"{\\\"title\\\":\\\"Dune Messiah\\\"}\");");
    assert_compiles("delete(store: \"docs\", entry: 1);");

    /* Table Mutations */
    assert_compiles("table_insert(store: \"users\", data: \"{\\\"name\\\":\\\"Alice\\\"}\");");
    assert_compiles("update(store: \"users\", entry: 10, changes: \"{\\\"name\\\":\\\"Bob\\\"}\");");

    /* Link Mutations */
    assert_compiles("create_link_type(name: \"authored\", cardinality: \"1:N\", enforced: true);");
    assert_compiles("link(type: 1, from: [10, 100], to: [20, 200], weight: 2.5, time: 5000);");
    assert_compiles("reinforce(type: 1, from: [10, 100], to: [20, 200], weight: 1.5);");
    assert_compiles("unlink(type: 1, from: [10, 100], to: [20, 200]);");

    /* Time Series Mutations */
    assert_compiles("series(store: \"metrics\", name: \"cpu\");");
    assert_compiles("append(store: \"metrics\", series: 1, time: 1000, value: 42.5);");
    assert_compiles("prune(store: \"metrics\", series: 1, before: 500);");
    assert_compiles("retention(store: \"metrics\", series: 1, age: 3600);");
    assert_compiles("apply_retention(store: \"metrics\", series: 1, time: 5000);");
    assert_compiles("downsample(source: [1, 1], into: [1, 2], step: 60, from: 0, to: 1000);");

    /* Vector Mutations */
    assert_compiles("vec_insert(store: \"emb\", data: \"\\x00\\x01\\x02\");");

    /* Blob & Maintenance */
    assert_compiles("free_blob(store: 1, page: 5, count: 1, len: 1024);");
    assert_compiles("compact(max: 100);");
    assert_compiles("reclaim();");
    assert_compiles("reconcile();");

    /* CRDT Replication */
    assert_compiles("peer(id: 2);");
    assert_compiles("sync(slice: \"slice_data\");");

    /* Piped Flux Sinks */
    assert_compiles("yield(\"{\\\"title\\\":\\\"Piped Document\\\"}\") | into(\"docs\");");
}

/* 3. Live engine execution test for Declarative SQL commands */
static void test_live_execution_declarative_sql(void)
{
    cdb_value ret;
    uint32_t peer_id = 0;

    db_test_open();

    /* Enclave Creation and Open */
    TEST_ASSERT_EQUAL_INT(CDB_OK, run_script(&db, "CREATE ENCLAVE 10 KEY \"env://CDB_TEST_ENC\";", &ret));
    TEST_ASSERT_EQUAL_INT(CDB_OK, run_script(&db, "OPEN ENCLAVE 10 KEY \"env://CDB_TEST_ENC\";", &ret));

    /* RBAC Principal Registration, Grant, and Permission Check */
    TEST_ASSERT_EQUAL_INT(CDB_OK, run_script(&db,
        "REGISTER PRINCIPAL 201 TENANT 1 PUBKEY \"0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798\";", &ret));
    TEST_ASSERT_EQUAL_INT(CDB_OK, run_script(&db, "GRANT ROLE 1 TO 201 SCOPE 0 PERMS 15;", &ret));
    TEST_ASSERT_EQUAL_INT(CDB_OK, run_script(&db, "CHECK PERM 201 SCOPE 0 PERMS 1;", &ret));

    /* Triggers */
    TEST_ASSERT_EQUAL_INT(CDB_OK, run_script(&db, "CREATE TRIGGER trg_test ON 100 AFTER INSERT EXECUTE \"proc_audit\" MAX DEPTH 8;", &ret));
    TEST_ASSERT_EQUAL_INT(CDB_OK, run_script(&db, "DROP TRIGGER trg_test;", &ret));

    /* Document pool creation & CRUD */
    TEST_ASSERT_EQUAL_INT(CDB_OK, run_script(&db, "CREATE STORE \"docstore\" MODEL document;", &ret));
    TEST_ASSERT_EQUAL_INT(CDB_OK, run_script(&db, "INSERT INTO \"docstore\" (\"{\\\"title\\\":\\\"Test 1\\\"}\");", &ret));
    TEST_ASSERT_EQUAL_INT(CDB_OK, run_script(&db, "PATCH \"docstore\" 1 WITH \"{\\\"rating\\\":5}\";", &ret));
    TEST_ASSERT_EQUAL_INT(CDB_OK, run_script(&db, "REPLACE \"docstore\" 1 WITH \"{\\\"title\\\":\\\"Test Replaced\\\"}\";", &ret));
    TEST_ASSERT_EQUAL_INT(CDB_OK, run_script(&db, "DELETE FROM \"docstore\" WHERE id = 1;", &ret));

    /* Maintenance */
    TEST_ASSERT_EQUAL_INT(CDB_OK, run_script(&db, "COMPACT MAX 10;", &ret));
    TEST_ASSERT_EQUAL_INT(CDB_OK, run_script(&db, "RECLAIM;", &ret));
    TEST_ASSERT_EQUAL_INT(CDB_OK, run_script(&db, "RECONCILE;", &ret));

    /* CRDT Peer */
    TEST_ASSERT_EQUAL_INT(CDB_OK, run_script(&db, "SET PEER 42;", &ret));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_get_peer_id(&db, &peer_id));
    TEST_ASSERT_EQUAL_UINT32(42, peer_id);

    db_test_close();
}

/* 4. Functional & Piped Flux commands */
static void test_live_execution_flux(void)
{
    cdb_value ret;
    uint32_t peer_id = 0;

    db_test_open();

    /* Enclave Creation */
    TEST_ASSERT_EQUAL_INT(CDB_OK, run_script(&db, "create_enclave(id: 20, key: \"env://CDB_TEST_ENC\");", &ret));
    TEST_ASSERT_EQUAL_INT(CDB_OK, run_script(&db, "open_enclave(id: 20, key: \"env://CDB_TEST_ENC\");", &ret));

    /* RBAC */
    TEST_ASSERT_EQUAL_INT(CDB_OK, run_script(&db,
        "register_principal(id: 301, tenant: 1, pubkey: \"0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798\");", &ret));
    TEST_ASSERT_EQUAL_INT(CDB_OK, run_script(&db, "grant_role(role: 2, to: 301, scope: 0, perms: 15);", &ret));
    TEST_ASSERT_EQUAL_INT(CDB_OK, run_script(&db, "check_perm(principal: 301, scope: 0, perms: 1);", &ret));

    /* Triggers */
    TEST_ASSERT_EQUAL_INT(CDB_OK, run_script(&db, "create_trigger(name: \"trg_flux\", on: 200, events: 1, execute: \"flux_audit\", max_depth: 8);", &ret));
    TEST_ASSERT_EQUAL_INT(CDB_OK, run_script(&db, "drop_trigger(\"trg_flux\");", &ret));

    /* Document Store & Mutations */
    TEST_ASSERT_EQUAL_INT(CDB_OK, run_script(&db, "create_store(name: \"flux_docs\", model: \"document\");", &ret));
    TEST_ASSERT_EQUAL_INT(CDB_OK, run_script(&db, "insert(store: \"flux_docs\", data: \"{\\\"msg\\\":\\\"Flux Hello\\\"}\");", &ret));
    TEST_ASSERT_EQUAL_INT(CDB_OK, run_script(&db, "patch(store: \"flux_docs\", entry: 1, patch: \"{\\\"v\\\":2}\");", &ret));
    TEST_ASSERT_EQUAL_INT(CDB_OK, run_script(&db, "replace(store: \"flux_docs\", entry: 1, data: \"{\\\"msg\\\":\\\"Flux Repl\\\"}\");", &ret));
    TEST_ASSERT_EQUAL_INT(CDB_OK, run_script(&db, "delete(store: \"flux_docs\", entry: 1);", &ret));

    /* Piped Flux into sink */
    TEST_ASSERT_EQUAL_INT(CDB_OK, run_script(&db, "yield(\"{\\\"msg\\\":\\\"Yielded Item\\\"}\") | into(\"flux_docs\");", &ret));

    /* Maintenance */
    TEST_ASSERT_EQUAL_INT(CDB_OK, run_script(&db, "compact(max: 5);", &ret));
    TEST_ASSERT_EQUAL_INT(CDB_OK, run_script(&db, "reclaim();", &ret));
    TEST_ASSERT_EQUAL_INT(CDB_OK, run_script(&db, "reconcile();", &ret));

    /* CRDT Peer */
    TEST_ASSERT_EQUAL_INT(CDB_OK, run_script(&db, "peer(id: 77);", &ret));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_get_peer_id(&db, &peer_id));
    TEST_ASSERT_EQUAL_UINT32(77, peer_id);

    db_test_close();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_compile_declarative_sql_parity);
    RUN_TEST(test_compile_flux_crud_parity);
    RUN_TEST(test_live_execution_declarative_sql);
    RUN_TEST(test_live_execution_flux);
    return UNITY_END();
}
