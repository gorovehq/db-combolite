"""ctypes bindings for the db-combolite C99 engine library (libcombolite.so).

Exposes typed C structures, engine entry points, constants, and zero-copy
value readers/writers.
"""
from __future__ import annotations

import ctypes
from ctypes import (
    POINTER, Structure, c_char, c_char_p, c_double, c_float, c_int, c_int8,
    c_int16, c_int32, c_int64, c_size_t, c_uint, c_uint8, c_uint16, c_uint32,
    c_uint64, c_void_p
)

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
CDB_OK = 0
CDB_E_INVALID_ARG = -1
CDB_E_BUFFER_TOO_SMALL = -2
CDB_E_TRUNCATED = -3
CDB_E_MALFORMED = -4
CDB_E_NOT_CANONICAL = -5
CDB_E_OVERFLOW = -6
CDB_E_DEPTH = -7
CDB_E_STATE = -8
CDB_E_UNSUPPORTED = -9
CDB_E_IO = -10
CDB_E_NOT_FOUND = -11
CDB_E_READ_ONLY = -12
CDB_E_CORRUPT = -13
CDB_E_EXISTS = -14
CDB_E_SCHEMA = -15
CDB_E_BUSY = -16
CDB_E_COLLISION = -17
CDB_E_TIMEOUT = -18
CDB_E_OUT_OF_BUDGET = -19
CDB_E_SECURITY = -20

# Data Models
CDB_MODEL_RELATIONAL = 1
CDB_MODEL_DOCUMENT = 2
CDB_MODEL_VECTOR = 3
CDB_MODEL_TIME_SERIES = 4
CDB_MODEL_LINK = 5

# Byte Order
CDB_ORDER_LITTLE = 0x4C  # 'L'
CDB_ORDER_BIG = 0x42     # 'B'

# Layout Flags
CDB_LAYOUT_ADDR64 = 0x01
CDB_LAYOUT_COMPACT_HEADER = 0x02

# Durability Levels
CDB_DURABILITY_DEFAULT = 0
CDB_DURABILITY_FULL = 1
CDB_DURABILITY_NORMAL = 2
CDB_DURABILITY_OFF = 3

# Store flags
CDB_STORE_DROPPED = 0x1

# Table constants
CDB_TABLE_KEY_MAX = 512
CDB_TABLE_NO_INDEX = 0xFFFFFFFF
CDB_TABLE_MAX_COLUMNS = 64

# Value types (cdb_value_type)
CDB_TYPE_NULL = 0
CDB_TYPE_BOOL = 1
CDB_TYPE_INT = 2
CDB_TYPE_F64 = 3
CDB_TYPE_F32 = 4
CDB_TYPE_TIMESTAMP = 5
CDB_TYPE_REF = 6
CDB_TYPE_STRING = 7
CDB_TYPE_BINARY = 8
CDB_TYPE_DECIMAL = 9
CDB_TYPE_ARRAY = 10
CDB_TYPE_OBJECT = 11
CDB_TYPE_VECTOR = 12
CDB_TYPE_BLOB_REF = 13
CDB_TYPE_UNKNOWN = 14

# Vector measures
CDB_VEC_BASE = 0
CDB_VEC_COSINE = 1
CDB_VEC_DOT = 2
CDB_VEC_EUCLIDEAN = 3
CDB_VEC_HAMMING = 4

CDB_VEC_AUTO = 0
CDB_VEC_EXACT = 1
CDB_VEC_APPROX = 2

CDB_VEC_F32 = 0

# Decimal mode
CDB_DECIMAL_UNBOUNDED = 0
CDB_DECIMAL_COMPACT = 1

# FTS modes
CDB_FTS_ANY = 1
CDB_FTS_ALL = 2

# Cardinality
CDB_CARD_1_1 = 1
CDB_CARD_1_N = 2
CDB_CARD_N_M = 3

# Link Directions
CDB_LINK_OUT = 0
CDB_LINK_IN = 1

# Link on delete
CDB_LINK_ON_DELETE_TOMBSTONE = 0
CDB_LINK_ON_DELETE_CASCADE = 1
CDB_LINK_ON_DELETE_REFUSE = 2

# Link Store Name
CDB_LINK_STORE_NAME = "$links"


# ---------------------------------------------------------------------------
# C Structures
# ---------------------------------------------------------------------------
class cdb_env_ops(Structure):
    _fields_ = [
        ("open", c_void_p),
        ("close", c_void_p),
        ("read", c_void_p),
        ("write", c_void_p),
        ("sync", c_void_p),
        ("truncate", c_void_p),
        ("size", c_void_p),
        ("exists", c_void_p),
        ("unlink", c_void_p),
        ("rename", c_void_p),
        ("mkdir", c_void_p),
        ("rmdir", c_void_p),
        ("now_ns", c_void_p),
        ("random", c_void_p),
        ("mutex_create", c_void_p),
        ("mutex_lock", c_void_p),
        ("mutex_unlock", c_void_p),
        ("mutex_destroy", c_void_p),
        ("cond_create", c_void_p),
        ("cond_wait", c_void_p),
        ("cond_signal", c_void_p),
        ("cond_broadcast", c_void_p),
        ("cond_destroy", c_void_p),
        ("thread_create", c_void_p),
        ("thread_join", c_void_p),
    ]


class cdb_env(Structure):
    _fields_ = [
        ("ops", POINTER(cdb_env_ops)),
        ("ctx", c_void_p),
    ]


class cdb_hostfile(Structure):
    _fields_ = [
        ("fd", c_int),
        ("size", c_uint64),
    ]


class cdb_hostenv(Structure):
    _fields_ = [
        ("files", cdb_hostfile * 16),
        ("used", c_int * 16),
    ]


class cdb_companion_slot(Structure):
    _fields_ = [
        ("type_id", c_uint16),
        ("compat_class", c_uint8),
        ("state", c_uint8),
        ("watermark", c_uint64),
    ]


class cdb_region_anchor(Structure):
    _fields_ = [
        ("feature_index", c_uint8),
        ("size_hint", c_uint32),
        ("anchor", c_uint64),
    ]


class cdb_header(Structure):
    _fields_ = [
        ("order", c_uint32),               # cdb_byte_order enum
        ("epoch", c_uint8),
        ("role", c_uint8),
        ("layout_flags", c_uint8),
        ("db_id", c_uint8 * 16),
        ("committed_seq", c_uint64),
        ("write_counter", c_uint64),
        ("page_seq_hwm", c_uint64),
        ("anchor_root", c_uint64),
        ("append_point", c_uint64),
        ("min_page_log2", c_uint8),
        ("max_page_log2", c_uint8),
        ("page_mode", c_uint8),
        ("default_page_log2", c_uint8),
        ("checksum_alg", c_uint8),
        ("aead_alg", c_uint8),
        ("ring_size", c_uint16),
        ("compat_flags", c_uint64),
        ("rocompat_flags", c_uint64),
        ("incompat_flags", c_uint64),
        ("app_id", c_uint32),
        ("creator_version", c_uint32),
        ("writer_version", c_uint32),
        ("created_us", c_uint64),
        ("key_ref", c_uint8 * 128),
        ("companions", cdb_companion_slot * 12),
        ("anchors", cdb_region_anchor * 4),
        ("commit_hwm", c_uint64),
        ("anchor_mirror_root", c_uint64),
        ("content_hash_alg", c_uint8),
        ("marks_root", c_uint64),
        ("tamper_tag", c_uint8 * 16),
    ]


class cdb_pager_node(Structure):
    _fields_ = [
        ("in_use", c_int),
        ("dirty", c_int),
        ("level", c_uint32),
        ("number", c_uint64),
        ("primary", POINTER(c_uint8)),
        ("mirror", POINTER(c_uint8)),
        ("phys_primary", c_uint64),
        ("phys_mirror", c_uint64),
        ("lru", c_uint64),
    ]


class cdb_free_extent(Structure):
    _fields_ = [
        ("start", c_uint64),
        ("len", c_uint64),
        ("freed_seq", c_uint64),
    ]


class cdb_pager_config(Structure):
    _fields_ = [
        ("arena", POINTER(c_uint8)),
        ("arena_size", c_size_t),
        ("nodes", POINTER(cdb_pager_node)),
        ("node_cap", c_size_t),
        ("free_list", POINTER(cdb_free_extent)),
        ("free_cap", c_size_t),
        ("page_cache", POINTER(c_uint8)),
        ("page_cache_size", c_size_t),
    ]


class cdb_index_entry(Structure):
    _fields_ = [
        ("lid", c_uint64),
        ("phys", c_uint64),
    ]


class cdb_index_work(Structure):
    _fields_ = [
        ("bytes", POINTER(c_uint8)),
        ("bytes_size", c_size_t),
        ("entries", POINTER(cdb_index_entry)),
        ("entries_cap", c_size_t),
    ]


class cdb_index(Structure):
    _fields_ = [
        ("pager", c_void_p),
        ("root", c_uint64),
        ("flags", c_uint8),
        ("store_id", c_uint64),
        ("w", cdb_index_work),
        ("max_key", c_size_t),
        ("max_value", c_size_t),
    ]


class cdb_index_cursor(Structure):
    _fields_ = [
        ("ix", POINTER(cdb_index)),
        ("depth", c_uint),
        ("lid", c_uint64 * 16),
        ("slot", c_uint32 * 16),
        ("page", POINTER(c_uint8)),
        ("done", c_int),
    ]


class cdb_fts_token(Structure):
    _fields_ = [
        ("p", POINTER(c_uint8)),
        ("len", c_uint32),
        ("tf", c_uint32),
    ]


class cdb_fts_path(Structure):
    _fields_ = [
        ("ids", c_uint64 * 8),
        ("depth", c_uint32),
    ]


class cdb_fts_config(Structure):
    _fields_ = [
        ("paths", cdb_fts_path * 8),
        ("npaths", c_uint32),
        ("summary", c_int),
    ]


class cdb_coord_config(Structure):
    # As in cdb_coord.h; zeros take the defaults
    _fields_ = [
        ("max_coords", c_uint32),
        ("beat_ns", c_int64),
        ("dead_after", c_uint32),
        ("settle_ns", c_int64),
        ("baton_wait_ns", c_int64),
    ]


class cdb_db_config(Structure):
    _fields_ = [
        ("pager", cdb_pager_config),
        ("record_buf", POINTER(c_uint8)),
        ("record_buf_size", c_size_t),
        ("apply", c_void_p),
        ("apply_ctx", c_void_p),
        ("durability", c_uint32),
        ("normal_sync_every", c_uint32),
        ("flags", c_uint32),
        ("catalog_work", cdb_index_work),
        ("catalog_page", POINTER(c_uint8)),
        ("doc_buffers", POINTER(c_uint8)),
        ("doc_max", c_size_t),
        ("checkpoint_every", c_uint32),
        ("doc_stage", POINTER(c_uint8)),
        ("doc_stage_size", c_size_t),
        ("idx_stage", POINTER(c_uint8)),
        ("idx_stage_size", c_size_t),
        ("fts_buffers", POINTER(c_uint8)),
        ("vec_buffers", POINTER(c_uint8)),
        ("vec_work", POINTER(c_uint8)),
        ("vec_work_size", c_size_t),
        ("vec_refresh_budget", c_uint32),
        ("vec_ef", c_uint32),
        ("vec_exact_below", c_uint64),
        ("vec_mode", c_uint32),
        ("fts_tokens", POINTER(cdb_fts_token)),
        ("fts_tokens_cap", c_size_t),
        ("ts_buffers", POINTER(c_uint8)),
        ("ts_work", POINTER(c_uint8)),
        ("ts_work_size", c_size_t),
        ("ts_collision", c_void_p),
        ("ts_collision_ctx", c_void_p),
        ("table_buffers", POINTER(c_uint8)),
        ("col_work", POINTER(c_uint8)),
        ("col_work_size", c_size_t),
        ("max_open", c_uint32),
        ("slot_mem", c_void_p),
        ("slot_mem_size", c_size_t),
        ("coord", cdb_coord_config),
        ("worker_threads", c_uint32),
        ("master_key_uri", c_char_p),
        ("is_production", c_int),
        ("aead_alg", c_uint8),
        ("key_provider", c_void_p),
        ("key_provider_ctx", c_void_p),
        ("rotation_interval_days", c_uint32),
        ("rotation_policy", c_uint32),
        ("signing_policy", c_uint32),
        ("identity", c_void_p),
        ("query_mem", c_size_t),   # host profile: a query's memory budget (spec 17.4); 0 = default
        ("vec_mem", c_size_t),     # host profile: a graph refresh's memory budget (spec 17.4); 0 = default
    ]


class cdb_db(Structure):
    # Opaque, and at least cdb_db_struct_size() bytes (about 31 KB today):
    # ComboliteLib checks, so the engine growing it can't overrun this
    _fields_ = [
        ("_opaque", c_uint8 * 65536),
    ]


class cdb_store_info(Structure):
    _fields_ = [
        ("id", c_uint64),
        ("name", c_char * 65),
        ("_pad_name", c_uint8 * 7),
        ("name_len", c_size_t),
        ("model", c_uint32),
        ("_pad_model", c_uint32),
        ("domain_id", c_uint64),
        ("page_log2", c_uint32),
        ("compression", c_uint32),
        ("schema_version", c_uint32),
        ("_pad_sv", c_uint32),
        ("next_field_id", c_uint64),
        ("created_commit", c_uint64),
        ("flags", c_uint32),
        ("_pad_flags", c_uint32),
        ("head_root", c_uint64),
        ("rev_root", c_uint64),
        ("next_entry_id", c_uint64),
        ("full_text", c_uint8 * 512),
        ("full_text_len", c_size_t),
        ("vector", c_uint8 * 512),
        ("vector_len", c_size_t),
    ]


class cdb_link_type_info(Structure):
    _fields_ = [
        ("id", c_uint64),
        ("name", c_char * 65),
        ("_pad_name", c_uint8 * 7),
        ("name_len", c_size_t),
        ("cardinality", c_uint8),
        ("enforced", c_uint8),
        ("on_delete", c_uint8),
        ("_pad_tail", c_uint8 * 5),
    ]


class cdb_table_column(Structure):
    _fields_ = [
        ("name", c_char_p),
        ("name_len", c_size_t),
        ("type", c_uint32),
        ("not_null", c_int),
        ("default_value", POINTER(c_uint8)),
        ("default_len", c_size_t),
    ]


class cdb_table_def(Structure):
    _fields_ = [
        ("columns", POINTER(cdb_table_column)),
        ("ncolumns", c_uint32),
        ("primary_key", POINTER(c_uint32)),
        ("npk", c_uint32),
        ("keep", c_uint32),
        ("layout", c_uint32),
        ("group_rows", c_uint32),
        ("chunk_cap", c_uint32),
    ]


class cdb_table_column_info(Structure):
    _fields_ = [
        ("field_id", c_uint64),
        ("type", c_uint32),
        ("not_null", c_int),
        ("has_default", c_int),
        ("added", c_uint32),
        ("primary", c_int),
        ("_pad", c_uint32),
    ]


class cdb_table_cursor(Structure):
    _fields_ = [
        ("key", c_uint8 * 520),
        ("len", c_size_t),
        ("entry", c_uint64),
        ("started", c_int),
        ("_pad", c_uint32),
    ]


class cdb_doc_info(Structure):
    _fields_ = [
        ("revision", c_uint64),
        ("kind", c_uint8),
        ("_pad1", c_uint8 * 7),
        ("commit_number", c_uint64),
        ("principal", c_uint64),
        ("schema_version", c_uint32),
        ("_pad2", c_uint32),
    ]


class cdb_fts_hit(Structure):
    _fields_ = [
        ("entry", c_uint64),
        ("score", c_float),
        ("_pad", c_uint32),
    ]


class cdb_vec_config(Structure):
    _fields_ = [
        ("elem", c_uint32),
        ("dims", c_uint32),
        ("measure", c_uint32),
        ("_pad1", c_uint32),
        ("model", c_char_p),
        ("model_len", c_size_t),
        ("model_version", c_char_p),
        ("model_version_len", c_size_t),
    ]


class cdb_vec_hit(Structure):
    _fields_ = [
        ("entry", c_uint64),
        ("distance", c_double),
        ("source_store", c_uint64),
        ("source_entry", c_uint64),
        ("source_state", c_uint32),
        ("_pad", c_uint32),
    ]


class cdb_ref(Structure):
    _fields_ = [
        ("store", c_uint64),
        ("entry", c_uint64),
        ("ts", c_int64),
        ("flags", c_uint32),
        ("_pad", c_uint32),
    ]


class cdb_link(Structure):
    _fields_ = [
        ("type", c_uint64),
        ("source", cdb_ref),
        ("target", cdb_ref),
        ("weight", c_float),
        ("_pad1", c_uint32),
        ("time", c_int64),
        ("flags", c_uint8),
        ("_pad2", c_uint8 * 3),
        ("removed", c_uint32),
    ]


class cdb_ts_tag(Structure):
    _fields_ = [
        ("key", c_char_p),
        ("key_len", c_size_t),
        ("value", c_char_p),
        ("value_len", c_size_t),
    ]


class cdb_ts_field(Structure):
    _fields_ = [
        ("id", c_uint64),
        ("type", c_uint8),
        ("_pad", c_uint8 * 7),
    ]


class cdb_ts_def(Structure):
    _fields_ = [
        ("precision", c_uint8),
        ("_pad1", c_uint8 * 7),
        ("window", c_int64),
        ("change_of_value", c_int),
        ("nfields", c_uint16),
        ("_pad2", c_uint8 * 2),
        ("fields", cdb_ts_field * 16),
    ]


class cdb_ts_in(Structure):
    _fields_ = [
        ("ts", c_int64),
        ("use_clock", c_int),
        ("_pad", c_uint32),
        ("values", c_void_p),
        ("len", c_size_t),
    ]


class cdb_ts_row(Structure):
    _fields_ = [
        ("ts", c_int64),
        ("off", c_size_t),
        ("len", c_size_t),
    ]


class cdb_compile_error(Structure):
    _fields_ = [
        ("line", c_uint32),
        ("col", c_uint32),
        ("msg", c_char * 128),
    ]


class cdb_value_writer(Structure):
    _fields_ = [
        ("_opaque", c_uint8 * 1336),
    ]


class _cdb_val_ts(Structure):
    _fields_ = [("precision", c_uint32), ("ticks", c_int64)]


class _cdb_val_ref(Structure):
    _fields_ = [("store_id", c_uint64), ("entry_id", c_uint64)]


class _cdb_val_decimal(Structure):
    _fields_ = [("scale", c_uint64), ("unscaled", POINTER(c_uint8)), ("unscaled_len", c_size_t)]


class _cdb_val_array(Structure):
    _fields_ = [("count", c_uint64), ("items", POINTER(c_uint8)), ("items_len", c_size_t)]


class _cdb_val_object(Structure):
    _fields_ = [
        ("count", c_uint64),
        ("id_width", c_uint8),
        ("off_width", c_uint8),
        ("_pad", c_uint8 * 6),
        ("directory", POINTER(c_uint8)),
        ("directory_len", c_size_t),
        ("values", POINTER(c_uint8)),
        ("values_len", c_size_t),
    ]


class _cdb_val_vector(Structure):
    _fields_ = [
        ("elem", c_uint32),
        ("_pad", c_uint32),
        ("dims", c_uint64),
        ("data", POINTER(c_uint8)),
        ("data_len", c_size_t),
    ]


class _cdb_val_blob(Structure):
    _fields_ = [
        ("hash", c_uint8 * 32),
        ("total_len", c_uint64),
        ("first_page", c_uint64),
        ("page_count", c_uint64),
        ("flags", c_uint32),
        ("_pad", c_uint8 * 4),
    ]


class _cdb_val_as(ctypes.Union):
    _fields_ = [
        ("boolean", c_int),
        ("i64", c_int64),
        ("f64", c_double),
        ("f32", c_float),
        ("ts", _cdb_val_ts),
        ("ref", _cdb_val_ref),
        ("decimal", _cdb_val_decimal),
        ("array", _cdb_val_array),
        ("object", _cdb_val_object),
        ("vector", _cdb_val_vector),
        ("blob", _cdb_val_blob),
    ]


class cdb_value(Structure):
    _fields_ = [
        ("type", c_uint32),
        ("tag", c_uint8),
        ("_pad1", c_uint8 * 3),
        ("start", POINTER(c_uint8)),
        ("encoded_len", c_size_t),
        ("payload", POINTER(c_uint8)),
        ("payload_len", c_size_t),
        ("as_", _cdb_val_as),
    ]


class cdb_value_iter(Structure):
    _fields_ = [
        ("pos", POINTER(c_uint8)),
        ("end", POINTER(c_uint8)),
        ("order", c_uint32),
        ("_pad1", c_uint32),
        ("dir_pos", POINTER(c_uint8)),
        ("dir_end", POINTER(c_uint8)),
        ("id_width", c_uint8),
        ("off_width", c_uint8),
        ("_pad2", c_uint8 * 6),
    ]


class cdb_exec_field(Structure):
    _fields_ = [
        ("name", c_char_p),
        ("name_len", c_size_t),
        ("val", cdb_value),
    ]


class cdb_exec_row(Structure):
    _fields_ = [
        ("store_id", c_uint64),
        ("entry_id", c_uint64),
        ("model", c_uint32),
        ("score", c_float),
        ("timestamp", c_int64),
        ("vector", POINTER(c_float)),
        ("vector_dim", c_size_t),
        ("record_data", POINTER(c_uint8)),
        ("record_len", c_size_t),
        ("has_right", c_int),
        ("right_store_id", c_uint64),
        ("right_entry_id", c_uint64),
        ("right_model", c_uint32),
        ("right_record_data", POINTER(c_uint8)),
        ("right_record_len", c_size_t),
        ("n_fields", c_size_t),
        ("fields", cdb_exec_field * 16),   # CDB_EXEC_MAX_FIELDS; last, as in cdb_exec.h
    ]


class cdb_exec_cursor(Structure):
    # Opaque, and at least cdb_exec_cursor_size() bytes: ComboliteLib checks
    _fields_ = [
        ("_opaque", c_uint8 * 4096),
    ]


class cdb_proc_vm(Structure):
    _fields_ = [
        ("_opaque", c_uint8 * 102720),
    ]


# ---------------------------------------------------------------------------
# Engine Library Binder
# ---------------------------------------------------------------------------
class ComboliteLib:
    """Loads and binds entry points from libcombolite.so."""

    def __init__(self, lib_path: str):
        self.lib = ctypes.CDLL(lib_path)
        self._bind()
        self._check_layouts()
        global _db_catalog_fn, _db_pager_fn
        for name in ("cdb_db_catalog", "cdb_db_pager"):
            fn = getattr(self.lib, name, None)
            if fn is not None:
                fn.argtypes = [c_void_p]
                fn.restype = c_void_p
        _db_catalog_fn = getattr(self.lib, "cdb_db_catalog", None)
        _db_pager_fn = getattr(self.lib, "cdb_db_pager", None)

    def _check_layouts(self):
        """The structs mirrored here must match the library: a mismatch would
        make the engine read and write past them. Libraries too old to export
        their sizes are taken as they are."""
        c = self.lib
        checks = [("cdb_db_config_size", cdb_db_config, "exact"), ("cdb_exec_row_size", cdb_exec_row, "exact"),
                  ("cdb_db_struct_size", cdb_db, "fits"), ("cdb_exec_cursor_size", cdb_exec_cursor, "fits")]
        for fn_name, cls, how in checks:
            fn = getattr(c, fn_name, None)
            if fn is None:
                continue
            fn.argtypes = []
            fn.restype = c_size_t
            want, have = fn(), ctypes.sizeof(cls)
            if (how == "exact" and want != have) or (how == "fits" and want > have):
                raise RuntimeError(f"libcombolite and the studio disagree on {cls.__name__}: "
                                   f"the library's is {want} bytes, the binding's {have}. Update binding.py.")

    def _bind(self):
        c = self.lib

        # Status
        c.cdb_status_str.argtypes = [c_int]
        c.cdb_status_str.restype = c_char_p

        # Buffer size helpers
        c.cdb_index_work_size.argtypes = [c_size_t]
        c.cdb_index_work_size.restype = c_size_t

        c.cdb_index_entries_needed.argtypes = [c_size_t]
        c.cdb_index_entries_needed.restype = c_size_t

        c.cdb_index_open.argtypes = [POINTER(cdb_index), c_void_p, c_uint64, POINTER(cdb_index_work)]
        c.cdb_index_open.restype = c_int

        c.cdb_index_seek.argtypes = [POINTER(cdb_index_cursor), POINTER(cdb_index), c_char_p, c_char_p, c_size_t]
        c.cdb_index_seek.restype = c_int

        c.cdb_index_next.argtypes = [
            POINTER(cdb_index_cursor), c_char_p, c_size_t, POINTER(c_size_t),
            c_void_p, c_size_t, POINTER(c_size_t), POINTER(c_int)
        ]
        c.cdb_index_next.restype = c_int

        c.cdb_doc_buffers_size.argtypes = [c_size_t, c_size_t]
        c.cdb_doc_buffers_size.restype = c_size_t

        c.cdb_fts_buffers_size.argtypes = [c_size_t, c_size_t]
        c.cdb_fts_buffers_size.restype = c_size_t

        c.cdb_fts_tokens_needed.argtypes = [c_size_t, c_size_t]
        c.cdb_fts_tokens_needed.restype = c_size_t

        c.cdb_table_buffers_size.argtypes = [c_size_t, c_size_t]
        c.cdb_table_buffers_size.restype = c_size_t

        c.cdb_vec_work_size.argtypes = [c_uint32, c_uint32, c_uint32]
        c.cdb_vec_work_size.restype = c_size_t

        c.cdb_ts_work_size.argtypes = []
        c.cdb_ts_work_size.restype = c_size_t

        # Hostenv
        c.cdb_hostenv_init.argtypes = [POINTER(cdb_hostenv), POINTER(cdb_env)]
        c.cdb_hostenv_init.restype = None

        # Database lifecycle
        c.cdb_db_open.argtypes = [POINTER(cdb_db), POINTER(cdb_env), c_char_p, POINTER(cdb_db_config), POINTER(cdb_header)]
        c.cdb_db_open.restype = c_int

        c.cdb_db_close.argtypes = [POINTER(cdb_db)]
        c.cdb_db_close.restype = c_int

        c.cdb_db_tick.argtypes = [POINTER(cdb_db)]
        c.cdb_db_tick.restype = c_int

        c.cdb_db_reconcile.argtypes = [POINTER(cdb_db)]
        c.cdb_db_reconcile.restype = c_int

        c.cdb_db_compact.argtypes = [POINTER(cdb_db), c_uint64, POINTER(c_uint64), POINTER(c_uint64)]
        c.cdb_db_compact.restype = c_int

        c.cdb_db_reclaim.argtypes = [POINTER(cdb_db), POINTER(c_uint64), POINTER(c_uint64)]
        c.cdb_db_reclaim.restype = c_int

        # Transactions
        c.cdb_tx_begin.argtypes = [POINTER(cdb_db), c_uint32]
        c.cdb_tx_begin.restype = c_int

        c.cdb_tx_commit.argtypes = [POINTER(cdb_db)]
        c.cdb_tx_commit.restype = c_int

        c.cdb_tx_abort.argtypes = [POINTER(cdb_db)]
        c.cdb_tx_abort.restype = c_int

        # Catalog & Stores
        c.cdb_db_create_store.argtypes = [POINTER(cdb_db), c_char_p, c_size_t, c_uint32, POINTER(c_uint64)]
        c.cdb_db_create_store.restype = c_int

        c.cdb_db_find_store.argtypes = [POINTER(cdb_db), c_char_p, c_size_t, POINTER(c_uint64), POINTER(c_int)]
        c.cdb_db_find_store.restype = c_int

        c.cdb_db_drop_store.argtypes = [POINTER(cdb_db), c_uint64]
        c.cdb_db_drop_store.restype = c_int

        # Catalog internals
        # Note: &db->catalog is at offset 2640 in cdb_db
        c.cdb_catalog_get_store.argtypes = [c_void_p, c_uint64, POINTER(cdb_store_info)]
        c.cdb_catalog_get_store.restype = c_int

        c.cdb_catalog_next_store_id.argtypes = [c_void_p]
        c.cdb_catalog_next_store_id.restype = c_uint64

        c.cdb_catalog_field_name.argtypes = [c_void_p, c_uint64, c_uint64, c_char_p, c_size_t, POINTER(c_size_t)]
        c.cdb_catalog_field_name.restype = c_int

        c.cdb_catalog_get_link_type.argtypes = [c_void_p, c_uint64, POINTER(cdb_link_type_info)]
        c.cdb_catalog_get_link_type.restype = c_int

        # Relational Tables
        c.cdb_table_create.argtypes = [POINTER(cdb_db), c_char_p, c_size_t, POINTER(cdb_table_def), POINTER(c_uint64)]
        c.cdb_table_create.restype = c_int

        c.cdb_table_describe.argtypes = [POINTER(cdb_db), c_uint64, POINTER(cdb_table_column_info), c_uint32, POINTER(c_uint32), POINTER(c_uint32), POINTER(c_uint32)]
        c.cdb_table_describe.restype = c_int

        c.cdb_table_column_id.argtypes = [POINTER(cdb_db), c_uint64, c_char_p, c_size_t, POINTER(c_uint64)]
        c.cdb_table_column_id.restype = c_int

        c.cdb_table_scan.argtypes = [POINTER(cdb_db), c_uint64, c_uint32, c_void_p, c_size_t, c_void_p, c_size_t, POINTER(cdb_table_cursor), POINTER(c_uint64), c_size_t, POINTER(c_size_t)]
        c.cdb_table_scan.restype = c_int

        c.cdb_table_get.argtypes = [POINTER(cdb_db), c_uint64, c_uint64, c_void_p, c_size_t, POINTER(c_size_t)]
        c.cdb_table_get.restype = c_int

        c.cdb_table_insert.argtypes = [POINTER(cdb_db), c_uint64, c_void_p, c_size_t, POINTER(c_uint64)]
        c.cdb_table_insert.restype = c_int

        c.cdb_table_update.argtypes = [POINTER(cdb_db), c_uint64, c_uint64, c_void_p, c_size_t]
        c.cdb_table_update.restype = c_int

        c.cdb_table_delete.argtypes = [POINTER(cdb_db), c_uint64, c_uint64]
        c.cdb_table_delete.restype = c_int

        # Documents
        c.cdb_doc_insert.argtypes = [POINTER(cdb_db), c_uint64, c_char_p, c_size_t, c_void_p, c_size_t, POINTER(c_uint64)]
        c.cdb_doc_insert.restype = c_int

        c.cdb_doc_replace.argtypes = [POINTER(cdb_db), c_uint64, c_uint64, c_char_p, c_size_t, c_void_p, c_size_t]
        c.cdb_doc_replace.restype = c_int

        c.cdb_doc_delete.argtypes = [POINTER(cdb_db), c_uint64, c_uint64]
        c.cdb_doc_delete.restype = c_int

        c.cdb_doc_get.argtypes = [POINTER(cdb_db), c_uint64, c_uint64, c_uint64, c_void_p, c_size_t, POINTER(c_size_t), c_char_p, c_size_t, POINTER(c_size_t), POINTER(cdb_doc_info)]
        c.cdb_doc_get.restype = c_int

        c.cdb_doc_head.argtypes = [POINTER(cdb_db), c_uint64, c_uint64, POINTER(c_uint64), POINTER(c_int)]
        c.cdb_doc_head.restype = c_int

        c.cdb_doc_next_live.argtypes = [POINTER(cdb_db), c_uint64, c_uint64, POINTER(c_uint64), POINTER(c_int)]
        c.cdb_doc_next_live.restype = c_int

        c.cdb_doc_search.argtypes = [POINTER(cdb_db), c_uint64, c_char_p, c_size_t, c_int, POINTER(cdb_fts_hit), c_size_t, POINTER(c_size_t)]
        c.cdb_doc_search.restype = c_int

        # Vectors
        c.cdb_vec_create.argtypes = [POINTER(cdb_db), c_char_p, c_size_t, POINTER(cdb_vec_config), POINTER(c_uint64)]
        c.cdb_vec_create.restype = c_int

        c.cdb_vec_describe.argtypes = [POINTER(cdb_db), c_uint64, POINTER(cdb_vec_config), c_char_p, c_char_p]
        c.cdb_vec_describe.restype = c_int

        c.cdb_vec_insert.argtypes = [POINTER(cdb_db), c_uint64, c_void_p, c_size_t, c_uint64, c_uint64, POINTER(c_uint64)]
        c.cdb_vec_insert.restype = c_int

        c.cdb_vec_replace.argtypes = [POINTER(cdb_db), c_uint64, c_uint64, c_void_p, c_size_t]
        c.cdb_vec_replace.restype = c_int

        c.cdb_vec_delete.argtypes = [POINTER(cdb_db), c_uint64, c_uint64]
        c.cdb_vec_delete.restype = c_int

        c.cdb_vec_get.argtypes = [POINTER(cdb_db), c_uint64, c_uint64, c_void_p, c_size_t, POINTER(c_size_t), POINTER(c_uint64), POINTER(c_uint64)]
        c.cdb_vec_get.restype = c_int

        c.cdb_vec_search.argtypes = [POINTER(cdb_db), c_uint64, c_void_p, c_size_t, c_uint32, POINTER(cdb_vec_hit), c_size_t, POINTER(c_size_t)]
        c.cdb_vec_search.restype = c_int

        c.cdb_vec_search_mode.argtypes = [POINTER(cdb_db), c_uint64, c_void_p, c_size_t, c_uint32, c_uint32, POINTER(cdb_vec_hit), c_size_t, POINTER(c_size_t)]
        c.cdb_vec_search_mode.restype = c_int

        c.cdb_vec_neighbours.argtypes = [POINTER(cdb_db), c_uint64, c_uint64, POINTER(cdb_vec_hit), c_size_t, POINTER(c_size_t), POINTER(c_int)]
        c.cdb_vec_neighbours.restype = c_int

        c.cdb_vec_create_graph.argtypes = [POINTER(cdb_db), c_uint64, c_uint32]
        c.cdb_vec_create_graph.restype = c_int

        c.cdb_vec_refresh.argtypes = [POINTER(cdb_db), c_uint64, c_size_t, POINTER(c_size_t)]
        c.cdb_vec_refresh.restype = c_int

        # Links
        c.cdb_link_define_type.argtypes = [POINTER(cdb_db), c_char_p, c_size_t, c_uint32, c_int, c_uint32, POINTER(c_uint64)]
        c.cdb_link_define_type.restype = c_int

        c.cdb_link_find_type.argtypes = [POINTER(cdb_db), c_char_p, c_size_t, POINTER(c_uint64), POINTER(c_int)]
        c.cdb_link_find_type.restype = c_int

        c.cdb_link_add.argtypes = [POINTER(cdb_db), c_uint64, cdb_ref, cdb_ref, c_float, c_int64]
        c.cdb_link_add.restype = c_int

        c.cdb_link_remove.argtypes = [POINTER(cdb_db), c_uint64, cdb_ref, cdb_ref]
        c.cdb_link_remove.restype = c_int

        c.cdb_link_get.argtypes = [POINTER(cdb_db), c_uint64, cdb_ref, cdb_ref, POINTER(cdb_link), POINTER(c_int)]
        c.cdb_link_get.restype = c_int

        c.cdb_link_list.argtypes = [POINTER(cdb_db), cdb_ref, c_int, c_uint64, POINTER(cdb_link), POINTER(cdb_link), c_size_t, POINTER(c_size_t)]
        c.cdb_link_list.restype = c_int

        # Time series
        c.cdb_ts_create.argtypes = [POINTER(cdb_db), c_char_p, c_size_t, c_uint32, c_int64, POINTER(c_uint64)]
        c.cdb_ts_create.restype = c_int

        c.cdb_ts_series.argtypes = [POINTER(cdb_db), c_uint64, c_char_p, c_size_t, POINTER(cdb_ts_tag), c_size_t, POINTER(cdb_ts_def), POINTER(c_uint64), POINTER(c_int)]
        c.cdb_ts_series.restype = c_int

        c.cdb_ts_describe.argtypes = [POINTER(cdb_db), c_uint64, c_uint64, POINTER(cdb_ts_def)]
        c.cdb_ts_describe.restype = c_int

        c.cdb_ts_append.argtypes = [POINTER(cdb_db), c_uint64, c_uint64, POINTER(cdb_ts_in), c_size_t]
        c.cdb_ts_append.restype = c_int

        c.cdb_ts_range.argtypes = [POINTER(cdb_db), c_uint64, c_uint64, c_int64, c_int64, POINTER(c_uint8), c_size_t, POINTER(cdb_ts_row), c_size_t, POINTER(c_size_t), POINTER(c_int64)]
        c.cdb_ts_range.restype = c_int

        c.cdb_ts_drop_before.argtypes = [POINTER(cdb_db), c_uint64, c_uint64, c_int64]
        c.cdb_ts_drop_before.restype = c_int

        # Query compilation & VM
        c.cdb_compile_query.argtypes = [c_char_p, c_size_t, c_void_p, POINTER(c_uint8), c_size_t, POINTER(c_size_t), POINTER(cdb_compile_error)]
        c.cdb_compile_query.restype = c_int

        c.cdb_compile_proc.argtypes = [c_char_p, c_size_t, c_void_p, POINTER(c_uint8), c_size_t, POINTER(c_size_t), POINTER(cdb_compile_error)]
        c.cdb_compile_proc.restype = c_int

        c.cdb_proc_vm_init.argtypes = [c_void_p, POINTER(cdb_db), c_uint32, c_void_p, c_uint8, c_uint64]
        c.cdb_proc_vm_init.restype = None

        c.cdb_proc_exec.argtypes = [c_void_p, POINTER(c_uint8), c_size_t, POINTER(cdb_value)]
        c.cdb_proc_exec.restype = c_int

        c.cdb_exec_open.argtypes = [POINTER(cdb_exec_cursor), POINTER(cdb_db), POINTER(c_uint8), c_size_t, c_void_p, c_size_t, POINTER(c_uint8), c_size_t]
        c.cdb_exec_open.restype = c_int

        c.cdb_exec_next.argtypes = [POINTER(cdb_exec_cursor), POINTER(cdb_exec_row), POINTER(c_int)]
        c.cdb_exec_next.restype = c_int

        c.cdb_exec_close.argtypes = [POINTER(cdb_exec_cursor)]
        c.cdb_exec_close.restype = None

        # Value readers & writers
        c.cdb_value_writer_init.argtypes = [POINTER(cdb_value_writer), POINTER(c_uint8), c_size_t, c_uint32, c_uint32]
        c.cdb_value_writer_init.restype = None

        c.cdb_value_writer_len.argtypes = [POINTER(cdb_value_writer)]
        c.cdb_value_writer_len.restype = c_size_t

        c.cdb_value_write_null.argtypes = [POINTER(cdb_value_writer)]
        c.cdb_value_write_null.restype = c_int

        c.cdb_value_write_bool.argtypes = [POINTER(cdb_value_writer), c_int]
        c.cdb_value_write_bool.restype = c_int

        c.cdb_value_write_int.argtypes = [POINTER(cdb_value_writer), c_int64]
        c.cdb_value_write_int.restype = c_int

        c.cdb_value_write_f64.argtypes = [POINTER(cdb_value_writer), c_double]
        c.cdb_value_write_f64.restype = c_int

        c.cdb_value_write_f32.argtypes = [POINTER(cdb_value_writer), c_float]
        c.cdb_value_write_f32.restype = c_int

        c.cdb_value_write_timestamp.argtypes = [POINTER(cdb_value_writer), c_uint32, c_int64]
        c.cdb_value_write_timestamp.restype = c_int

        c.cdb_value_write_string.argtypes = [POINTER(cdb_value_writer), c_char_p, c_size_t]
        c.cdb_value_write_string.restype = c_int

        c.cdb_value_write_vector.argtypes = [POINTER(cdb_value_writer), c_uint32, c_uint64, c_void_p]
        c.cdb_value_write_vector.restype = c_int

        c.cdb_value_begin_object.argtypes = [POINTER(cdb_value_writer)]
        c.cdb_value_begin_object.restype = c_int

        c.cdb_value_field.argtypes = [POINTER(cdb_value_writer), c_uint64]
        c.cdb_value_field.restype = c_int

        c.cdb_value_end_object.argtypes = [POINTER(cdb_value_writer)]
        c.cdb_value_end_object.restype = c_int

        c.cdb_value_read.argtypes = [POINTER(c_uint8), c_size_t, c_uint32, POINTER(cdb_value)]
        c.cdb_value_read.restype = c_int

        c.cdb_value_object_iter.argtypes = [POINTER(cdb_value), c_uint32, POINTER(cdb_value_iter)]
        c.cdb_value_object_iter.restype = c_int

        c.cdb_value_object_next.argtypes = [POINTER(cdb_value_iter), POINTER(c_uint64), POINTER(cdb_value), POINTER(c_int)]
        c.cdb_value_object_next.restype = c_int


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def status_str(lib: ComboliteLib, status: int) -> str:
    res = lib.lib.cdb_status_str(status)
    if res:
        return res.decode("utf-8")
    return f"Status {status}"


# cdb_db_catalog / cdb_db_pager from the loaded library (ComboliteLib sets
# them); None for libraries too old to export them
_db_catalog_fn = None
_db_pager_fn = None


def get_catalog_ptr(db_ptr: POINTER(cdb_db)) -> c_void_p:
    """The cdb_catalog inside cdb_db: asked of the library, or for an older
    library the offset it had (16344)."""
    if _db_catalog_fn is not None:
        return c_void_p(_db_catalog_fn(db_ptr))
    return c_void_p(ctypes.cast(db_ptr, c_void_p).value + 16344)


def get_pager_ptr(db_ptr: POINTER(cdb_db)) -> c_void_p:
    """The cdb_pager inside cdb_db: asked of the library, or for an older
    library the offset it had (12640)."""
    if _db_pager_fn is not None:
        return c_void_p(_db_pager_fn(db_ptr))
    return c_void_p(ctypes.cast(db_ptr, c_void_p).value + 12640)


def decode_value(val: cdb_value, lib: ComboliteLib, order: int = CDB_ORDER_LITTLE) -> object:
    """Decodes a cdb_value into a Python object."""
    t = val.type
    if t == CDB_TYPE_NULL:
        return None
    elif t == CDB_TYPE_BOOL:
        return bool(val.as_.boolean)
    elif t == CDB_TYPE_INT:
        return val.as_.i64
    elif t == CDB_TYPE_F64:
        return val.as_.f64
    elif t == CDB_TYPE_F32:
        return val.as_.f32
    elif t == CDB_TYPE_TIMESTAMP:
        return val.as_.ts.ticks
    elif t == CDB_TYPE_STRING:
        if not val.payload or val.payload_len == 0:
            return ""
        return ctypes.string_at(val.payload, val.payload_len).decode("utf-8", errors="replace")
    elif t == CDB_TYPE_BINARY:
        if not val.payload or val.payload_len == 0:
            return b""
        return bytes(ctypes.string_at(val.payload, val.payload_len))
    elif t == CDB_TYPE_VECTOR:
        elem = val.as_.vector.elem
        dims = val.as_.vector.dims
        data = val.as_.vector.data
        if not data or dims == 0:
            return []
        if elem == 0:  # CDB_VEC_F32
            data_ptr = ctypes.cast(data, POINTER(c_float))
            return [float(data_ptr[i]) for i in range(dims)]
        elif elem == 3:  # CDB_VEC_I8
            data_ptr = ctypes.cast(data, POINTER(c_int8))
            return [float(data_ptr[i]) for i in range(dims)]
        elif elem == 4:  # CDB_VEC_BINARY (packed bits, LSB first)
            data_ptr = ctypes.cast(data, POINTER(c_uint8))
            return [float((data_ptr[i // 8] >> (i % 8)) & 1) for i in range(dims)]
        elif elem == 1:  # CDB_VEC_F16
            try:
                import numpy as _np
                raw = ctypes.string_at(data, dims * 2)
                return [float(x) for x in _np.frombuffer(raw, dtype=_np.float16)]
            except Exception:
                data_ptr = ctypes.cast(data, POINTER(c_uint16))
                return [float(data_ptr[i]) for i in range(dims)]
        else:
            data_ptr = ctypes.cast(data, POINTER(c_float))
            return [float(data_ptr[i]) for i in range(dims)]
    elif t == CDB_TYPE_OBJECT:
        out = {}
        it = cdb_value_iter()
        lib.lib.cdb_value_object_iter(ctypes.byref(val), order, ctypes.byref(it))
        f_id = c_uint64()
        f_val = cdb_value()
        done = c_int(0)
        while True:
            lib.lib.cdb_value_object_next(ctypes.byref(it), ctypes.byref(f_id), ctypes.byref(f_val), ctypes.byref(done))
            if done.value:
                break
            out[f_id.value] = decode_value(f_val, lib, order)
        return out
    elif t == CDB_TYPE_ARRAY:
        out = []
        it = cdb_value_iter()
        lib.lib.cdb_value_array_iter(ctypes.byref(val), order, ctypes.byref(it))
        item = cdb_value()
        done = c_int(0)
        while True:
            lib.lib.cdb_value_array_next(ctypes.byref(it), ctypes.byref(item), ctypes.byref(done))
            if done.value:
                break
            out.append(decode_value(item, lib, order))
        return out
    return None

