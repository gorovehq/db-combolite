/* Catalog: the database's description of itself (spec 4, 14.3).
 *
 * The body is one B+tree, kept twice (primary and mirror, spec 3.5), whose
 * keys start with a kind byte:
 *   01 + ID                      -> object record
 *   02 + kind + name             -> ID (lookup by name)
 *   03 + store ID + version      -> schema version record
 *   04 + store ID + field name   -> field ID (field dictionary, spec 14.6.3)
 *   05 + store ID + field ID     -> field name
 * Records live in their own pages, written twice; index values name both.
 *
 * Catalog changes commit immediately: each call that changes the catalog
 * ends with a pager commit, which writes a new root slot (spec 3.2 step 3). */
#ifndef CDB_CATALOG_H
#define CDB_CATALOG_H

#include "cdb_index.h"
#include "cdb_pager.h"

/* Object kinds. */
#define CDB_OBJ_STORE 1u
#define CDB_OBJ_LINK_TYPE 2u

/* Data models (spec 6-10). */
#define CDB_MODEL_RELATIONAL 1u
#define CDB_MODEL_DOCUMENT 2u
#define CDB_MODEL_VECTOR 3u
#define CDB_MODEL_TIME_SERIES 4u
#define CDB_MODEL_LINK 5u /* the one link store (cdb_link.h): a reserved name, starting with '$' */

/* Store flags. */
#define CDB_STORE_DROPPED 0x1u

#define CDB_CATALOG_NAME_MAX 64u
#define CDB_CATALOG_FULL_TEXT_MAX 512u
#define CDB_CATALOG_SETTINGS_MAX 512u

typedef struct cdb_store_info {
    uint64_t id;
    char name[CDB_CATALOG_NAME_MAX + 1];
    size_t name_len;
    uint32_t model;
    uint64_t domain_id;       /* 0 = public, until enclaves exist */
    uint32_t page_log2;
    uint32_t compression;
    uint32_t schema_version;  /* current version; 0 = no schema */
    uint64_t next_field_id;
    uint64_t created_commit;
    uint32_t flags;
    /* Document pools (spec 8, 14.9). Optional fields; 0 when absent. */
    uint64_t head_root;       /* index: entry ID -> head revision */
    uint64_t rev_root;        /* index: (entry ID, revision) -> revision page */
    uint64_t next_entry_id;   /* entry IDs are never reused (spec 6.1) */
    uint8_t full_text[CDB_CATALOG_FULL_TEXT_MAX]; /* encoded settings (14.14); empty = none */
    size_t full_text_len;
    uint8_t vector[CDB_CATALOG_SETTINGS_MAX]; /* encoded collection settings (14.12.3); empty = none */
    size_t vector_len;
} cdb_store_info;

/* A link type (spec 6.7, 6.9, 14.13): a catalog object of its own kind. */
typedef struct cdb_link_type_info {
    uint64_t id;
    char name[CDB_CATALOG_NAME_MAX + 1];
    size_t name_len;
    uint8_t cardinality; /* CDB_CARD_* (cdb_link.h) */
    uint8_t enforced;    /* 1: checked on every write; 0: recorded only */
    uint8_t on_delete;   /* CDB_LINK_ON_DELETE_* (cdb_link.h) */
} cdb_link_type_info;

typedef struct cdb_catalog {
    cdb_pager *pager;
    cdb_index body;
    cdb_index mirror;
    cdb_index_work work;
    uint8_t *page;            /* one page buffer */
    int open;                 /* body and mirror exist */
} cdb_catalog;

/* Binds the catalog to an open pager. page is a page_size buffer; work is
 * index working memory (shared by the body and its mirror). */
cdb_status cdb_catalog_open(cdb_catalog *cat, cdb_pager *p, const cdb_index_work *work, uint8_t *page);

cdb_status cdb_catalog_create_store(cdb_catalog *cat, const char *name, size_t len, uint32_t model,
                                    uint64_t *id);
/* As create_store, with a vector collection's settings (an encoded object)
 * in the same record and commit. */
cdb_status cdb_catalog_create_store_ex(cdb_catalog *cat, const char *name, size_t len, uint32_t model,
                                       const void *vector, size_t vector_len, uint64_t *id);
/* The ID the next created store will get, for pages written before it. */
uint64_t cdb_catalog_next_store_id(const cdb_catalog *cat);
/* Replaces a vector collection's settings and commits immediately. */
cdb_status cdb_catalog_set_vector(cdb_catalog *cat, uint64_t store_id, const void *settings, size_t len);
/* Records a store's next entry ID without committing (like stage_pool). */
cdb_status cdb_catalog_stage_next_entry(cdb_catalog *cat, uint64_t store_id, uint64_t next_entry_id);

/* *found is 0 when no live store has that name. */
cdb_status cdb_catalog_find_store(cdb_catalog *cat, const char *name, size_t len, uint64_t *id, int *found);
/* Dropped stores are still found by ID (their IDs are never reused). */
cdb_status cdb_catalog_get_store(cdb_catalog *cat, uint64_t id, cdb_store_info *info);
cdb_status cdb_catalog_rename_store(cdb_catalog *cat, uint64_t id, const char *name, size_t len);
cdb_status cdb_catalog_drop_store(cdb_catalog *cat, uint64_t id);

/* Link types. create assigns the next object ID and commits; the name is
 * unique among link types. get gives CDB_E_NOT_FOUND for an ID that is not
 * a link type. */
cdb_status cdb_catalog_create_link_type(cdb_catalog *cat, const cdb_link_type_info *in, uint64_t *id);
cdb_status cdb_catalog_find_link_type(cdb_catalog *cat, const char *name, size_t len, uint64_t *id, int *found);
cdb_status cdb_catalog_get_link_type(cdb_catalog *cat, uint64_t id, cdb_link_type_info *out);

/* Records a pool's index roots and entry counter without committing: the
 * caller's next pager commit (reconciliation) makes it durable. */
cdb_status cdb_catalog_stage_pool(cdb_catalog *cat, uint64_t store_id, uint64_t head_root,
                                  uint64_t rev_root, uint64_t next_entry_id);

/* Records a store's full-text settings (an encoded object, spec 14.14) and
 * commits immediately. */
cdb_status cdb_catalog_set_full_text(cdb_catalog *cat, uint64_t store_id, const void *settings, size_t len);

/* Schema history (spec 4.3, 4.11): adds a version and makes it current. */
cdb_status cdb_catalog_add_schema(cdb_catalog *cat, uint64_t store_id, const void *schema, size_t len,
                                  uint32_t *version);
cdb_status cdb_catalog_get_schema(cdb_catalog *cat, uint64_t store_id, uint32_t version,
                                  void *buf, size_t cap, size_t *len);

/* Field dictionary (spec 14.6.3): returns the field's ID, adding the name if new. */
cdb_status cdb_catalog_field_id(cdb_catalog *cat, uint64_t store_id, const char *name, size_t len,
                                uint64_t *field_id);
cdb_status cdb_catalog_field_name(cdb_catalog *cat, uint64_t store_id, uint64_t field_id,
                                  char *name, size_t cap, size_t *len);

#endif
