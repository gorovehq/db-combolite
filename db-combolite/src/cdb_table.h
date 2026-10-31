/* Relational tables, row-oriented (spec 9, 14.10.1).
 *
 * A table is a document pool (cdb_doc.h) with a table schema (spec 9.7):
 * each row is an encoded object (spec 14.6) whose field IDs are the table's
 * column IDs. Rows get revisions, transactions and crash safety from the
 * document engine; this layer adds the columns, the constraints and the
 * indexes. The document calls refuse a table, so rows can only be written
 * here, where every change is checked.
 *
 * Columns (spec 9.8) live in the catalog's schema history: each schema
 * version lists the columns with their field ID, type, not-null flag,
 * optional constant default and the version that added them. Rows are never
 * rewritten by a schema change; a row is read through the current columns:
 *   - a field that is no longer a column is hidden;
 *   - a column added after the row was written reads as its default, or as
 *     null without one; a column the row simply lacks is null;
 *   - a widened column (int to decimal, f32 to f64) is converted.
 * Nulls are not stored: a row holds only its non-null columns.
 *
 * Constraints (spec 9.9) are checked when the change is made, against the
 * table and its indexes as the open transaction sees them: types, not-null,
 * unique indexes and the primary key. Nulls never collide in a unique index.
 * Applying a committed change never refuses one.
 *
 * Indexes (spec 9.10): the primary key (index 0 when there is one) and up to
 * CDB_TABLE_MAX_INDEXES in all, over 1 to CDB_TABLE_MAX_KEY_COLUMNS columns
 * of indexable types (everything but decimal, array, object and vector).
 * Applying each change keeps them current. Key layout: the columns' values in
 * the order-preserving encoding, then for a non-unique index, or a key with a
 * null, the entry ID; the value is the entry ID.
 *
 * History (spec 9.6, 9.11): keep 0 keeps every revision (history on); keep N
 * keeps the last N revisions of each row, and a delete removes the row.
 *
 * Schema and index changes commit immediately, like other catalog changes,
 * so they are not allowed inside a transaction. Creating a table takes
 * several catalog commits; a crash part-way leaves a table without columns,
 * which every row call refuses with CDB_E_STATE and which can be dropped.
 *
 * Buffers: cdb_db_config.table_buffers, cdb_table_buffers_size() bytes. */
#ifndef CDB_TABLE_H
#define CDB_TABLE_H

#include "combolite/cdb_value.h"
#include "cdb_db.h"

#define CDB_TABLE_KEY_MAX 512u        /* longest index key built (also capped by the index) */
#define CDB_TABLE_NO_INDEX UINT32_MAX /* scan in entry order instead of by an index */

/* A column as given to create or add_column. */
typedef struct cdb_table_column {
    const char *name;
    size_t name_len;
    cdb_value_type type;          /* CDB_TYPE_*, not NULL or UNKNOWN */
    int not_null;
    const uint8_t *default_value; /* one encoded value of the column's type, or NULL */
    size_t default_len;
} cdb_table_column;

#define CDB_TABLE_LAYOUT_ROW 0u
#define CDB_TABLE_LAYOUT_COLUMN 1u

typedef struct cdb_table_def {
    const cdb_table_column *columns;
    unsigned ncolumns;
    const unsigned *primary_key;  /* positions in columns[]; NULL for none */
    unsigned npk;
    uint32_t keep;                /* 0: every revision; N: the last N (spec 9.11) */
    /* Column-oriented tables (spec 9.12; cdb_rowgroup.h). They need
     * cdb_db_config.col_work. Row calls on them are not built yet and
     * return CDB_E_UNSUPPORTED. */
    unsigned layout;              /* CDB_TABLE_LAYOUT_* */
    uint32_t group_rows;          /* 0: CDB_RG_ROWS_DEFAULT */
    uint32_t chunk_cap;           /* 0: CDB_RG_CAP_DEFAULT; also the largest row the table takes */
} cdb_table_def;

/* A column as described. */
typedef struct cdb_table_column_info {
    uint64_t field_id;
    cdb_value_type type;
    int not_null;
    int has_default;
    uint32_t added;               /* schema version that added it */
    int primary;                  /* part of the primary key */
} cdb_table_column_info;

/* Where a scan stopped; zero it to start. */
typedef struct cdb_table_cursor {
    uint8_t key[CDB_TABLE_KEY_MAX + 8u];
    size_t len;
    uint64_t entry;
    int started;
} cdb_table_cursor;

size_t cdb_table_buffers_size(size_t page_size, size_t doc_max);

/* Creates a table. CDB_E_EXISTS: the name is taken; CDB_E_INVALID_ARG: a bad
 * column, duplicate names, a default of the wrong type, a primary-key column
 * that cannot be indexed. Primary-key columns become not-null. */
cdb_status cdb_table_create(cdb_db *db, const char *name, size_t len, const cdb_table_def *def, uint64_t *store);

/* The current columns, in field-ID order; *n is their count (at most cap
 * are copied). Also the current schema version and keep setting. */
cdb_status cdb_table_describe(cdb_db *db, uint64_t store, cdb_table_column_info *cols, unsigned cap,
                              unsigned *n, uint32_t *version, uint32_t *keep);
/* A column's field ID by name; CDB_E_NOT_FOUND if no current column has it. */
cdb_status cdb_table_column_id(cdb_db *db, uint64_t store, const char *name, size_t len, uint64_t *field_id);

/* Schema changes (spec 9.8), each a new schema version except rename.
 * add: CDB_E_EXISTS if a current column has the name; a not-null column
 * needs a default. drop: CDB_E_STATE while an index uses the column.
 * widen: int to decimal or f32 to f64 only, else CDB_E_INVALID_ARG. */
cdb_status cdb_table_add_column(cdb_db *db, uint64_t store, const cdb_table_column *col, uint64_t *field_id);
cdb_status cdb_table_drop_column(cdb_db *db, uint64_t store, uint64_t field_id);
cdb_status cdb_table_rename_column(cdb_db *db, uint64_t store, uint64_t field_id, const char *name, size_t len);
cdb_status cdb_table_widen_column(cdb_db *db, uint64_t store, uint64_t field_id, cdb_value_type type);
/* Changes the revision window; rows are pruned as they next change. */
cdb_status cdb_table_set_keep(cdb_db *db, uint64_t store, uint32_t keep);

/* Indexes (spec 9.10). create builds the index over the existing rows
 * before returning; CDB_E_EXISTS if a unique index finds a duplicate. Its
 * number stays until it is dropped; numbers are not reused while it lives. */
cdb_status cdb_table_create_index(cdb_db *db, uint64_t store, const uint64_t *fields, unsigned n, int unique,
                                  uint32_t *index);
cdb_status cdb_table_drop_index(cdb_db *db, uint64_t store, uint32_t index);

/* Rows, inside a transaction. A row is an encoded object keyed by column
 * field ID. insert fills in defaults for columns it leaves out; replace and
 * update do not (a column left out of a replace is null). update changes
 * only the columns its object names (a null value sets the column null).
 * Errors (nothing is journaled on any of them):
 *   CDB_E_STATE        no open transaction, or a table without columns
 *   CDB_E_INVALID_ARG  not a table, or not an encoded object
 *   CDB_E_SCHEMA       an unknown column, a value of the wrong type, or a
 *                      not-null column left null
 *   CDB_E_EXISTS       a unique index or the primary key already has the key
 *   CDB_E_NOT_FOUND    replace, update or delete of a row that does not exist
 *   CDB_E_OVERFLOW     the row, or one of its index keys, is too large */
cdb_status cdb_table_insert(cdb_db *db, uint64_t store, const void *row, size_t len, uint64_t *entry);
cdb_status cdb_table_replace(cdb_db *db, uint64_t store, uint64_t entry, const void *row, size_t len);
cdb_status cdb_table_update(cdb_db *db, uint64_t store, uint64_t entry, const void *changes, size_t len);
cdb_status cdb_table_delete(cdb_db *db, uint64_t store, uint64_t entry);

/* Reads see committed rows only, through the current columns.
 * get: CDB_E_NOT_FOUND for no such row; CDB_E_BUFFER_TOO_SMALL with *len set.
 * find: the row whose primary key is `key`, an encoded array of values in
 * key-column order; *found 0 if none.
 * scan: entries in index order (or entry order with CDB_TABLE_NO_INDEX)
 * from the key `from` (inclusive) up to `to` (inclusive, compared on the
 * columns it gives), each an encoded array of values for the index's leading
 * columns, or NULL for no bound. At most max entries; *n of them; call again
 * with the same cursor for more until *n is 0. */
cdb_status cdb_table_get(cdb_db *db, uint64_t store, uint64_t entry, void *out, size_t cap, size_t *len);
cdb_status cdb_table_find(cdb_db *db, uint64_t store, const void *key, size_t key_len, uint64_t *entry,
                          int *found);
cdb_status cdb_table_scan(cdb_db *db, uint64_t store, uint32_t index, const void *from, size_t from_len,
                          const void *to, size_t to_len, cdb_table_cursor *cur, uint64_t *entries, size_t max,
                          size_t *n);

/* Engine hooks. */
cdb_status cdb_table_keep_setting(const uint8_t *settings, size_t len, cdb_byte_order o, uint32_t *keep);
cdb_status cdb_table_apply_before(cdb_db *db, uint64_t store, uint64_t entry);
cdb_status cdb_table_apply_after(cdb_db *db, uint64_t store, uint64_t entry);
extern const cdb_slot_ops cdb_table_slot_ops; /* open tables (cdb_slots.h) */

/* Column-oriented tables: sets g up over the table's row groups, with
 * cfg.col_work. CDB_E_INVALID_ARG if the table is row-oriented. */
struct cdb_rg;
cdb_status cdb_table_rowgroups(cdb_db *db, uint64_t store, struct cdb_rg *g);
/* A column-chunk nibblet moved (cdb_nib_merge). CDB_E_NOT_FOUND if its
 * table is gone or no group refers to it. */
cdb_status cdb_table_nib_moved(cdb_db *db, uint64_t store, const uint8_t *okey, size_t oklen, uint64_t addr);

#endif
