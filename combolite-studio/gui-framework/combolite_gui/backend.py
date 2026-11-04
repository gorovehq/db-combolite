"""Backend adapter layer.

The GUI only ever talks to a `Backend`. `MockBackend` holds fake, mutable, in-memory
data so every view and edit works today. Fill in `NativeBackend` with real calls into
the C library (ctypes / cffi / a CPython extension) as the engine API settles.

Conventions the GUI relies on:
  * table_schema() rows start with (column_name, type, ...); column 0 of table rows is the primary key.
  * Raise BackendError("message") for user-facing failures (constraint violations etc.).
  * Unimplemented methods raise NotImplementedError; the GUI reports it, never crashes.
"""
from __future__ import annotations

import json
import math
import random
import re
import time
from dataclasses import dataclass, field
from enum import Enum


class Model(str, Enum):
    TABLE = "table"
    DOCUMENT = "document"
    VECTOR = "vector"
    LINK = "link"
    TIMESERIES = "timeseries"


# model -> (sidebar heading, singular label, symbolic icon)
MODEL_INFO = {
    Model.TABLE: ("TABLES", "Table", "x-office-spreadsheet-symbolic"),
    Model.DOCUMENT: ("DOCUMENT POOLS", "Document pool", "text-x-generic-symbolic"),
    Model.VECTOR: ("VECTOR COLLECTIONS", "Vector collection", "view-app-grid-symbolic"),
    Model.LINK: ("LINKS", "Link set", "insert-link-symbolic"),
    Model.TIMESERIES: ("TIME SERIES", "Time series", "document-open-recent-symbolic"),
}


@dataclass
class Store:
    name: str
    model: Model
    count: int
    size_bytes: int = 0
    meta: dict = field(default_factory=dict)


@dataclass
class Result:
    columns: list
    rows: list
    total: int | None = None
    elapsed_ms: float = 0.0
    message: str = ""


@dataclass
class DocHit:
    id: str
    title: str
    rev: int
    score: float | None = None


@dataclass
class Procedure:
    name: str
    params: list      # e.g. ["$year", "$lang"]
    body: str         # CMBQL text
    doc: str = ""
    handler: Any = None


@dataclass
class VectorGraph:
    ids: list            # entry ids
    vectors: list        # list[list[float]], same order as ids
    edges: list          # list[(i, j)] index pairs from the neighbour graph
    labels: list = field(default_factory=list)  # what each vector points to, same order


class BackendError(Exception):
    """User-facing error; the GUI shows the message in a toast."""


class Backend:
    name = "base"
    path: str | None = None

    @property
    def is_open(self) -> bool:
        return self.path is not None

    def _todo(self, what):
        raise NotImplementedError(f"{type(self).__name__}.{what} is not wired up yet")

    # lifecycle
    def open(self, path: str) -> None: self._todo("open")
    def create(self, path: str) -> None: self._todo("create")
    def close(self) -> None: self.path = None
    def file_info(self) -> dict: return {"Path": self.path or ""}
    def list_stores(self) -> list[Store]: return []

    # relational tables
    def table_schema(self, name: str) -> Result: self._todo("table_schema")
    def table_indexes(self, name: str) -> Result: self._todo("table_indexes")
    def table_rows(self, name: str, offset: int, limit: int, filter_text: str = "") -> Result: self._todo("table_rows")
    def table_insert(self, name: str, values: dict) -> object: self._todo("table_insert")        # -> new pk
    def table_update(self, name: str, pk, column: str, value) -> None: self._todo("table_update")
    def table_delete(self, name: str, pks: list) -> int: self._todo("table_delete")              # -> rows deleted

    # document pools
    def doc_list(self, pool: str, limit: int = 200) -> list[DocHit]: self._todo("doc_list")
    def fts_search(self, pool: str, query: str, limit: int = 50) -> list[DocHit]: self._todo("fts_search")
    def doc_revisions(self, pool: str, doc_id: str) -> list[int]: self._todo("doc_revisions")
    def doc_get(self, pool: str, doc_id: str, rev: int | None = None) -> dict: self._todo("doc_get")
    def doc_put(self, pool: str, doc_id: str, doc: dict) -> int: self._todo("doc_put")           # -> new rev
    def doc_create(self, pool: str, doc: dict) -> str: self._todo("doc_create")                  # -> new id
    def doc_delete(self, pool: str, doc_id: str) -> None: self._todo("doc_delete")

    # vector collections
    def vector_search(self, coll: str, query: str, k: int, approximate: bool = False) -> list[tuple[str, float]]:
        self._todo("vector_search")
    def vector_graph(self, coll: str, limit: int = 220) -> VectorGraph: self._todo("vector_graph")
    def vector_get(self, coll: str, vec_id: str) -> list[float]: self._todo("vector_get")
    def vector_put(self, coll: str, vec_id: str, vec: list[float]) -> None: self._todo("vector_put")
    def vector_delete(self, coll: str, vec_id: str) -> None: self._todo("vector_delete")
    def vector_source_label(self, coll: str, vec_id: str) -> str: return ""   # what the vector points to

    # links
    def link_edges(self, name: str, limit: int | None = 500) -> Result: self._todo("link_edges")       # columns: source, target, ...
    def link_add(self, name: str, source: str, target: str, weight: float = 1.0) -> None: self._todo("link_add")
    def link_delete(self, name: str, pairs: list[tuple[str, str]]) -> int: self._todo("link_delete")

    # time series
    def ts_series_list(self, name: str) -> list[str]: return []
    def ts_points(self, name: str, window_s: int, series: str | None = None) -> list[tuple[float, float]]: self._todo("ts_points")
    def ts_append(self, name: str, t: float, value: float, series: str | None = None) -> None: self._todo("ts_append")

    # query language (CMBQL)
    def execute(self, text: str) -> Result: self._todo("execute")
    def explain(self, text: str) -> Result: self._todo("explain")

    # stored procedures
    def proc_list(self) -> list[Procedure]: self._todo("proc_list")
    def proc_save(self, proc: Procedure) -> None: self._todo("proc_save")
    def proc_delete(self, name: str) -> None: self._todo("proc_delete")


class NativeBackend(Backend):
    """Bridge to the real C99 library.

    The engine never heap-allocates: all arena/page buffers are allocated here
    (ctypes.create_string_buffer) and kept alive on self for the handle's lifetime.
    """

    name = "native"
    PAGE_SIZE = 4096
    NODES = 512
    DOC_MAX = 2097152       # 2 MiB per document body
    DOC_STAGE = 1048576     # 1 MiB doc staging
    VEC_DIMS = 384
    VEC_K = 16

    def __init__(self, lib_path: str | None = None):
        import ctypes
        import os
        from pathlib import Path
        from .binding import (
            ComboliteLib, status_str, get_catalog_ptr, get_pager_ptr, decode_value,
            cdb_db, cdb_hostenv, cdb_env, cdb_header, cdb_db_config, cdb_pager_node,
            cdb_free_extent, cdb_index, cdb_index_cursor, cdb_index_entry, cdb_fts_token, cdb_table_column,
            cdb_table_column_info, cdb_table_cursor, cdb_doc_info, cdb_fts_hit,
            cdb_vec_config, cdb_vec_hit, cdb_ref, cdb_link, cdb_ts_tag, cdb_ts_def,
            cdb_ts_in, cdb_ts_row, cdb_compile_error, cdb_value_writer, cdb_value,
            cdb_value_iter, cdb_store_info, cdb_link_type_info, cdb_table_def,
            cdb_exec_cursor, cdb_exec_row, cdb_proc_vm,
            CDB_OK, CDB_E_NOT_FOUND, CDB_E_BUFFER_TOO_SMALL,
            CDB_MODEL_RELATIONAL, CDB_MODEL_DOCUMENT, CDB_MODEL_VECTOR,
            CDB_MODEL_TIME_SERIES, CDB_MODEL_LINK, CDB_ORDER_LITTLE, CDB_ORDER_BIG,
            CDB_LAYOUT_ADDR64, CDB_DURABILITY_DEFAULT, CDB_DURABILITY_FULL,
            CDB_STORE_DROPPED, CDB_TABLE_NO_INDEX, CDB_DECIMAL_COMPACT,
            CDB_VEC_BASE, CDB_VEC_COSINE, CDB_VEC_DOT, CDB_VEC_EUCLIDEAN, CDB_VEC_HAMMING,
            CDB_VEC_F32, CDB_VEC_AUTO, CDB_VEC_EXACT, CDB_VEC_APPROX,
            CDB_TYPE_NULL, CDB_TYPE_BOOL, CDB_TYPE_INT, CDB_TYPE_F64, CDB_TYPE_F32,
            CDB_TYPE_TIMESTAMP, CDB_TYPE_REF, CDB_TYPE_STRING, CDB_TYPE_BINARY,
            CDB_TYPE_VECTOR, CDB_TYPE_OBJECT, CDB_TYPE_ARRAY,
            CDB_FTS_ANY, CDB_FTS_ALL, CDB_CARD_1_1, CDB_CARD_1_N, CDB_CARD_N_M,
            CDB_LINK_OUT, CDB_LINK_STORE_NAME
        )

        self.ctypes = ctypes
        self.b_types = {
            "status_str": status_str,
            "get_catalog_ptr": get_catalog_ptr,
            "get_pager_ptr": get_pager_ptr,
            "decode_value": decode_value,
            "cdb_db": cdb_db,
            "cdb_hostenv": cdb_hostenv,
            "cdb_env": cdb_env,
            "cdb_header": cdb_header,
            "cdb_db_config": cdb_db_config,
            "cdb_pager_node": cdb_pager_node,
            "cdb_free_extent": cdb_free_extent,
            "cdb_index": cdb_index,
            "cdb_index_cursor": cdb_index_cursor,
            "cdb_index_entry": cdb_index_entry,
            "cdb_fts_token": cdb_fts_token,
            "cdb_table_column": cdb_table_column,
            "cdb_table_column_info": cdb_table_column_info,
            "cdb_table_cursor": cdb_table_cursor,
            "cdb_doc_info": cdb_doc_info,
            "cdb_fts_hit": cdb_fts_hit,
            "cdb_vec_config": cdb_vec_config,
            "cdb_vec_hit": cdb_vec_hit,
            "cdb_ref": cdb_ref,
            "cdb_link": cdb_link,
            "cdb_ts_tag": cdb_ts_tag,
            "cdb_ts_def": cdb_ts_def,
            "cdb_ts_in": cdb_ts_in,
            "cdb_ts_row": cdb_ts_row,
            "cdb_compile_error": cdb_compile_error,
            "cdb_value_writer": cdb_value_writer,
            "cdb_value": cdb_value,
            "cdb_value_iter": cdb_value_iter,
            "cdb_store_info": cdb_store_info,
            "cdb_link_type_info": cdb_link_type_info,
            "cdb_table_def": cdb_table_def,
            "cdb_exec_cursor": cdb_exec_cursor,
            "cdb_exec_row": cdb_exec_row,
            "cdb_proc_vm": cdb_proc_vm,
            "CDB_OK": CDB_OK,
            "CDB_E_NOT_FOUND": CDB_E_NOT_FOUND,
            "CDB_E_BUFFER_TOO_SMALL": CDB_E_BUFFER_TOO_SMALL,
            "CDB_MODEL_RELATIONAL": CDB_MODEL_RELATIONAL,
            "CDB_MODEL_DOCUMENT": CDB_MODEL_DOCUMENT,
            "CDB_MODEL_VECTOR": CDB_MODEL_VECTOR,
            "CDB_MODEL_TIME_SERIES": CDB_MODEL_TIME_SERIES,
            "CDB_MODEL_LINK": CDB_MODEL_LINK,
            "CDB_ORDER_LITTLE": CDB_ORDER_LITTLE,
            "CDB_ORDER_BIG": CDB_ORDER_BIG,
            "CDB_LAYOUT_ADDR64": CDB_LAYOUT_ADDR64,
            "CDB_DURABILITY_DEFAULT": CDB_DURABILITY_DEFAULT,
            "CDB_DURABILITY_FULL": CDB_DURABILITY_FULL,
            "CDB_STORE_DROPPED": CDB_STORE_DROPPED,
            "CDB_TABLE_NO_INDEX": CDB_TABLE_NO_INDEX,
            "CDB_DECIMAL_COMPACT": CDB_DECIMAL_COMPACT,
            "CDB_VEC_BASE": CDB_VEC_BASE,
            "CDB_VEC_COSINE": CDB_VEC_COSINE,
            "CDB_VEC_DOT": CDB_VEC_DOT,
            "CDB_VEC_EUCLIDEAN": CDB_VEC_EUCLIDEAN,
            "CDB_VEC_HAMMING": CDB_VEC_HAMMING,
            "CDB_VEC_F32": CDB_VEC_F32,
            "CDB_VEC_AUTO": CDB_VEC_AUTO,
            "CDB_VEC_EXACT": CDB_VEC_EXACT,
            "CDB_VEC_APPROX": CDB_VEC_APPROX,
            "CDB_TYPE_NULL": CDB_TYPE_NULL,
            "CDB_TYPE_BOOL": CDB_TYPE_BOOL,
            "CDB_TYPE_INT": CDB_TYPE_INT,
            "CDB_TYPE_F64": CDB_TYPE_F64,
            "CDB_TYPE_F32": CDB_TYPE_F32,
            "CDB_TYPE_TIMESTAMP": CDB_TYPE_TIMESTAMP,
            "CDB_TYPE_REF": CDB_TYPE_REF,
            "CDB_TYPE_STRING": CDB_TYPE_STRING,
            "CDB_TYPE_BINARY": CDB_TYPE_BINARY,
            "CDB_TYPE_VECTOR": CDB_TYPE_VECTOR,
            "CDB_TYPE_OBJECT": CDB_TYPE_OBJECT,
            "CDB_TYPE_ARRAY": CDB_TYPE_ARRAY,
            "CDB_FTS_ANY": CDB_FTS_ANY,
            "CDB_FTS_ALL": CDB_FTS_ALL,
            "CDB_CARD_1_1": CDB_CARD_1_1,
            "CDB_CARD_1_N": CDB_CARD_1_N,
            "CDB_CARD_N_M": CDB_CARD_N_M,
            "CDB_LINK_OUT": CDB_LINK_OUT,
            "CDB_LINK_STORE_NAME": CDB_LINK_STORE_NAME,
        }

        resolved_lib = lib_path or os.environ.get("COMBOLITE_LIB")
        if not resolved_lib:
            # Check package-bundled library and standard build directory locations
            pkg_dir = Path(__file__).resolve().parent
            repo_root = Path(__file__).resolve().parents[3]
            candidates = [
                pkg_dir / "libcombolite.so",
                pkg_dir / ".libs" / "libcombolite.so",
                repo_root / "db-combolite" / "build-shared" / "libcombolite.so",
                repo_root / "db-combolite" / "build-rel" / "libcombolite.so",
                repo_root / "db-combolite" / "build" / "libcombolite.so",
                Path("/usr/local/lib/libcombolite.so"),
                Path("/usr/lib/libcombolite.so"),
            ]
            for c in candidates:
                if c.exists():
                    resolved_lib = str(c)
                    break
        if not resolved_lib:
            import ctypes.util
            found = ctypes.util.find_library("combolite")
            if found:
                resolved_lib = found
        if not resolved_lib:
            raise BackendError("COMBOLITE_LIB is not set and libcombolite.so was not found in build or system directories")

        self.clib = ComboliteLib(resolved_lib)
        self.path = None
        self._db = None
        self._buffers = []
        self._he = None
        self._env = None
        # The library table records each ingested book, so these are plain
        # queries (GutenFree Diet's layout)
        self._procs = {
            "ingestion_records": Procedure(
                "ingestion_records", [], "from library",
                "Every book whose text is stored: passages, words, passage keys, when ingested and embedded"),
            "downloaded_books": Procedure(
                "downloaded_books", [], "select ebook, title, author, passages, first_passage, last_passage from library",
                "The downloaded books with their passage counts and key ranges"),
        }
        self._link_cache = {}
        self._field_name_cache = {}
        self._label_cache = {}
        self._vec_count_cache = {}
        self._label_body = None
        self._ts_extra = {}

    def _alloc(self, size: int):
        buf = self.ctypes.create_string_buffer(size)
        self._buffers.append(buf)
        return self.ctypes.cast(buf, self.ctypes.POINTER(self.ctypes.c_uint8))

    def _normalize_base(self, path: str) -> str:
        p = path.rstrip("/")
        for ext in (".cdbarch", ".combolite", ".cmb", ".db"):
            if p.endswith(ext):
                p = p[:-len(ext)]
                break
        return p

    def _init_engine_config(self):
        c = self.clib.lib
        ct = self.ctypes
        self._buffers.clear()

        page = self.PAGE_SIZE
        nodes = self.NODES
        doc_max = self.DOC_MAX
        doc_stage = self.DOC_STAGE
        vec_dims = self.VEC_DIMS
        vec_k = self.VEC_K

        cfg = self.b_types["cdb_db_config"]()
        cfg.pager.node_cap = nodes
        cfg.pager.arena_size = nodes * 2 * page + 2 * page
        cfg.pager.arena = self._alloc(cfg.pager.arena_size)
        cfg.pager.nodes = ct.cast(self._alloc(nodes * ct.sizeof(self.b_types["cdb_pager_node"])),
                                  ct.POINTER(self.b_types["cdb_pager_node"]))
        cfg.pager.free_cap = 65536
        cfg.pager.free_list = ct.cast(self._alloc(cfg.pager.free_cap * ct.sizeof(self.b_types["cdb_free_extent"])),
                                      ct.POINTER(self.b_types["cdb_free_extent"]))

        cfg.record_buf_size = doc_max + 2 * page + 64
        cfg.record_buf = self._alloc(cfg.record_buf_size)

        cfg.catalog_work.bytes_size = c.cdb_index_work_size(page)
        cfg.catalog_work.bytes = self._alloc(cfg.catalog_work.bytes_size)
        cfg.catalog_work.entries_cap = c.cdb_index_entries_needed(page)
        cfg.catalog_work.entries = ct.cast(
            self._alloc(cfg.catalog_work.entries_cap * ct.sizeof(self.b_types["cdb_index_entry"])),
            ct.POINTER(self.b_types["cdb_index_entry"])
        )
        cfg.catalog_page = self._alloc(page)

        cfg.doc_max = doc_max
        cfg.doc_buffers = self._alloc(c.cdb_doc_buffers_size(page, doc_max))
        cfg.doc_stage_size = doc_stage
        cfg.doc_stage = self._alloc(doc_stage)
        cfg.fts_buffers = self._alloc(c.cdb_fts_buffers_size(page, doc_max))
        cfg.fts_tokens_cap = c.cdb_fts_tokens_needed(page, doc_max)
        cfg.fts_tokens = ct.cast(self._alloc(cfg.fts_tokens_cap * ct.sizeof(self.b_types["cdb_fts_token"])),
                                 ct.POINTER(self.b_types["cdb_fts_token"]))
        cfg.table_buffers = self._alloc(c.cdb_table_buffers_size(page, doc_max))

        cfg.vec_buffers = self._alloc(3 * page)
        cfg.vec_work_size = c.cdb_vec_work_size(self.b_types["CDB_VEC_F32"], vec_dims, vec_k)
        cfg.vec_work = self._alloc(cfg.vec_work_size)

        cfg.ts_buffers = self._alloc(3 * page)
        cfg.ts_work_size = c.cdb_ts_work_size()
        cfg.ts_work = self._alloc(cfg.ts_work_size)

        cfg.durability = self.b_types["CDB_DURABILITY_FULL"]
        return cfg

    def _open_or_create(self, path: str, create: bool) -> None:
        self.close()
        self._field_name_cache = {}
        self._label_cache = {}
        self._vec_count_cache = {}
        base = self._normalize_base(path)

        self._he = self.b_types["cdb_hostenv"]()
        self._env = self.b_types["cdb_env"]()
        self.clib.lib.cdb_hostenv_init(self.ctypes.byref(self._he), self.ctypes.byref(self._env))

        cfg = self._init_engine_config()
        self._cfg = cfg

        hdr = None
        if create:
            hdr = self.b_types["cdb_header"]()
            hdr.order = self.b_types["CDB_ORDER_LITTLE"]
            hdr.layout_flags = self.b_types["CDB_LAYOUT_ADDR64"]
            hdr.default_page_log2 = 12
            for i in range(16):
                hdr.db_id[i] = (i + int(time.time())) & 0xFF

        self._db = self.b_types["cdb_db"]()
        s = self.clib.lib.cdb_db_open(
            self.ctypes.byref(self._db),
            self.ctypes.byref(self._env),
            base.encode("utf-8"),
            self.ctypes.byref(cfg),
            self.ctypes.byref(hdr) if hdr else None
        )
        if s != self.b_types["CDB_OK"]:
            err_msg = self.b_types["status_str"](self.clib, s)
            self._db = None
            raise BackendError(f"Failed to {'create' if create else 'open'} database: {err_msg}")

        self.path = path

    def open(self, path: str) -> None:
        self._open_or_create(path, create=False)

    def create(self, path: str) -> None:
        self._open_or_create(path, create=True)

    def close(self) -> None:
        if self._db is not None:
            self.clib.lib.cdb_db_close(self.ctypes.byref(self._db))
            self._db = None
        self._buffers.clear()
        self._he = None
        self._env = None
        self.path = None

    def file_info(self) -> dict:
        if not self.is_open:
            return {"Path": ""}
        import os
        base = self._normalize_base(self.path)
        arch_path = f"{base}.cdbarch"
        sz_str = "0 B"
        if os.path.exists(arch_path):
            sz = os.path.getsize(arch_path)
            if sz > 1024 * 1024:
                sz_str = f"{sz / (1024 * 1024):.1f} MiB"
            elif sz > 1024:
                sz_str = f"{sz / 1024:.1f} KiB"
            else:
                sz_str = f"{sz} B"
        return {
            "Path": self.path,
            "Archive": arch_path,
            "Size": sz_str,
            "Page size": "4096 B",
            "Format": "v0 (C99 native)",
            "Journal": "write-ahead/rollback",
            "Last checkpoint": time.strftime("%Y-%m-%d %H:%M")
        }

    def _find_store_id(self, name: str) -> int:
        sid = self.ctypes.c_uint64()
        found = self.ctypes.c_int(0)
        nb = name.encode("utf-8")
        s = self.clib.lib.cdb_db_find_store(self.ctypes.byref(self._db), nb, len(nb),
                                            self.ctypes.byref(sid), self.ctypes.byref(found))
        if s != self.b_types["CDB_OK"] or not found.value:
            raise BackendError(f"No store named '{name}'")
        return sid.value

    def _store_names_map(self) -> dict[int, str]:
        if not self.is_open:
            return {}
        c = self.clib.lib
        ct = self.ctypes
        cat_ptr = self.b_types["get_catalog_ptr"](ct.byref(self._db))
        next_id = c.cdb_catalog_next_store_id(cat_ptr)
        mapping = {}
        for sid in range(1, next_id):
            info = self.b_types["cdb_store_info"]()
            if c.cdb_catalog_get_store(cat_ptr, sid, ct.byref(info)) == self.b_types["CDB_OK"]:
                mapping[sid] = ct.string_at(info.name, info.name_len).decode("utf-8")
        return mapping

    def _get_links_store_info(self):
        if not self.is_open:
            return None
        c = self.clib.lib
        ct = self.ctypes
        cat_ptr = self.b_types["get_catalog_ptr"](ct.byref(self._db))
        next_id = c.cdb_catalog_next_store_id(cat_ptr)
        for sid in range(1, next_id):
            info = self.b_types["cdb_store_info"]()
            if c.cdb_catalog_get_store(cat_ptr, sid, ct.byref(info)) == self.b_types["CDB_OK"]:
                if info.model == self.b_types["CDB_MODEL_LINK"] and not (info.flags & self.b_types["CDB_STORE_DROPPED"]):
                    return info
        return None

    def _count_links_by_type(self, head_root: int) -> dict[int, int]:
        c = self.clib.lib
        ct = self.ctypes
        counts = {}
        pager_ptr = self.b_types["get_pager_ptr"](ct.byref(self._db))
        fwd_ix = self.b_types["cdb_index"]()
        if c.cdb_index_open(ct.byref(fwd_ix), pager_ptr, head_root, ct.byref(self._cfg.catalog_work)) != self.b_types["CDB_OK"]:
            return counts

        cur = self.b_types["cdb_index_cursor"]()
        page_buf = ct.create_string_buffer(self.PAGE_SIZE)
        if c.cdb_index_seek(ct.byref(cur), ct.byref(fwd_ix), page_buf, None, 0) != self.b_types["CDB_OK"]:
            return counts

        k_buf = ct.create_string_buffer(512)
        v_buf = ct.create_string_buffer(128)
        klen = ct.c_size_t()
        vlen = ct.c_size_t()
        done = ct.c_int(0)
        import struct

        while c.cdb_index_next(ct.byref(cur), k_buf, 512, ct.byref(klen), v_buf, 128, ct.byref(vlen), ct.byref(done)) == self.b_types["CDB_OK"] and not done.value:
            raw_k = k_buf.raw[:klen.value]
            if klen.value == 40:
                lt = struct.unpack(">Q", raw_k[16:24])[0]
            elif klen.value == 58:
                lt = struct.unpack(">Q", raw_k[25:33])[0]
            else:
                continue
            counts[lt] = counts.get(lt, 0) + 1
        return counts

    def _get_ts_store_summary(self, sid: int, info) -> dict:
        c = self.clib.lib
        ct = self.ctypes
        val = self.b_types["cdb_value"]()
        if c.cdb_value_read(ct.cast(info.vector, ct.POINTER(ct.c_uint8)), info.vector_len,
                            self.b_types["CDB_ORDER_LITTLE"], ct.byref(val)) != self.b_types["CDB_OK"]:
            return {"total_points": 0, "meta": {"unit": "pts", "late_arrival": "accept", "series": []}}

        meta_obj = self.b_types["decode_value"](val, self.clib)
        if not isinstance(meta_obj, dict):
            return {"total_points": 0, "meta": {"unit": "pts", "late_arrival": "accept", "series": []}}

        keys_root = meta_obj.get(3, 0)
        blocks_root = meta_obj.get(5, 0)
        pager_ptr = self.b_types["get_pager_ptr"](ct.byref(self._db))

        series_names = []
        series_map = {}
        if keys_root:
            keys_ix = self.b_types["cdb_index"]()
            if c.cdb_index_open(ct.byref(keys_ix), pager_ptr, keys_root, ct.byref(self._cfg.catalog_work)) == self.b_types["CDB_OK"]:
                cur = self.b_types["cdb_index_cursor"]()
                page_buf = ct.create_string_buffer(self.PAGE_SIZE)
                if c.cdb_index_seek(ct.byref(cur), ct.byref(keys_ix), page_buf, None, 0) == self.b_types["CDB_OK"]:
                    k_buf = ct.create_string_buffer(512)
                    v_buf = ct.create_string_buffer(128)
                    klen = ct.c_size_t()
                    vlen = ct.c_size_t()
                    done = ct.c_int(0)
                    import struct
                    while c.cdb_index_next(ct.byref(cur), k_buf, 512, ct.byref(klen), v_buf, 128, ct.byref(vlen), ct.byref(done)) == self.b_types["CDB_OK"] and not done.value:
                        ser_id = struct.unpack(">Q", v_buf.raw[:8])[0]
                        raw_k = k_buf.raw[:klen.value]
                        s_name = raw_k[1:].rstrip(b"\x00").decode("utf-8", errors="replace")
                        series_map[s_name] = ser_id
                        series_names.append(s_name)

        total_pts = 0
        series_point_counts = {}
        if blocks_root:
            blk_ix = self.b_types["cdb_index"]()
            if c.cdb_index_open(ct.byref(blk_ix), pager_ptr, blocks_root, ct.byref(self._cfg.catalog_work)) == self.b_types["CDB_OK"]:
                cur = self.b_types["cdb_index_cursor"]()
                page_buf = ct.create_string_buffer(self.PAGE_SIZE)
                if c.cdb_index_seek(ct.byref(cur), ct.byref(blk_ix), page_buf, None, 0) == self.b_types["CDB_OK"]:
                    k_buf = ct.create_string_buffer(512)
                    v_buf = ct.create_string_buffer(128)
                    klen = ct.c_size_t()
                    vlen = ct.c_size_t()
                    done = ct.c_int(0)
                    import struct
                    while c.cdb_index_next(ct.byref(cur), k_buf, 512, ct.byref(klen), v_buf, 128, ct.byref(vlen), ct.byref(done)) == self.b_types["CDB_OK"] and not done.value:
                        if vlen.value >= 12:
                            ser_id = struct.unpack(">Q", k_buf.raw[:8])[0]
                            pts = struct.unpack(">I", v_buf.raw[vlen.value - 12:vlen.value - 8])[0]
                            total_pts += pts
                            series_point_counts[ser_id] = series_point_counts.get(ser_id, 0) + pts

        series_names.sort(key=lambda name: series_point_counts.get(series_map.get(name, 0), 0), reverse=True)
        return {
            "total_points": total_pts,
            "meta": {
                "unit": "pts",
                "late_arrival": "accept",
                "series": series_names,
                "series_map": series_map,
                "series_counts": {name: series_point_counts.get(series_map[name], 0) for name in series_names}
            }
        }

    def list_stores(self) -> list[Store]:
        if not self.is_open:
            return []
        c = self.clib.lib
        ct = self.ctypes
        cat_ptr = self.b_types["get_catalog_ptr"](ct.byref(self._db))
        next_id = c.cdb_catalog_next_store_id(cat_ptr)

        link_counts = {}
        links_info = self._get_links_store_info()
        if links_info and links_info.head_root:
            link_counts = self._count_links_by_type(links_info.head_root)

        defined_link_types = []
        for sid in range(1, next_id):
            lt = self.b_types["cdb_link_type_info"]()
            s = c.cdb_catalog_get_link_type(cat_ptr, sid, ct.byref(lt))
            if s == self.b_types["CDB_OK"]:
                lt_name = ct.string_at(lt.name, lt.name_len).decode("utf-8")
                card = "n:m" if lt.cardinality == self.b_types["CDB_CARD_N_M"] else ("1:n" if lt.cardinality == self.b_types["CDB_CARD_1_N"] else "1:1")
                cnt = link_counts.get(sid, 0)
                defined_link_types.append(Store(lt_name, Model.LINK, cnt, meta={"cardinality": card}))

        stores = []
        for sid in range(1, next_id):
            info = self.b_types["cdb_store_info"]()
            s = c.cdb_catalog_get_store(cat_ptr, sid, ct.byref(info))
            if s != self.b_types["CDB_OK"] or (info.flags & self.b_types["CDB_STORE_DROPPED"]):
                continue

            name = ct.string_at(info.name, info.name_len).decode("utf-8")
            model_val = info.model
            if model_val == self.b_types["CDB_MODEL_RELATIONAL"]:
                # Count rows
                cur = self.b_types["cdb_table_cursor"]()
                ct.memset(ct.byref(cur), 0, ct.sizeof(cur))
                entries = (ct.c_uint64 * 1024)()
                n = ct.c_size_t(0)
                tot = 0
                while True:
                    c.cdb_table_scan(ct.byref(self._db), sid, self.b_types["CDB_TABLE_NO_INDEX"],
                                     None, 0, None, 0, ct.byref(cur), entries, 1024, ct.byref(n))
                    tot += n.value
                    if n.value == 0:
                        break
                stores.append(Store(name, Model.TABLE, tot))
            elif model_val == self.b_types["CDB_MODEL_DOCUMENT"]:
                tot = 0
                after = ct.c_uint64(0)
                entry = ct.c_uint64(0)
                done = ct.c_int(0)
                while True:
                    s_nxt = c.cdb_doc_next_live(ct.byref(self._db), sid, after.value, ct.byref(entry), ct.byref(done))
                    if s_nxt != self.b_types["CDB_OK"] or done.value:
                        break
                    tot += 1
                    after.value = entry.value
                meta = {"fts": "bm25"} if info.full_text_len else {}
                stores.append(Store(name, Model.DOCUMENT, tot, meta=meta))
            elif model_val == self.b_types["CDB_MODEL_VECTOR"]:
                cfg_v = self.b_types["cdb_vec_config"]()
                m_buf = ct.create_string_buffer(64)
                mv_buf = ct.create_string_buffer(64)
                c.cdb_vec_describe(ct.byref(self._db), sid, ct.byref(cfg_v), m_buf, mv_buf)
                tot, has_graph = self._vec_census(sid, int(info.next_entry_id or 0))
                meta = {"dims": cfg_v.dims,
                        "metric": {1: "cosine", 2: "dot", 3: "euclidean", 4: "hamming"}.get(cfg_v.measure, "?"),
                        "index": "neighbour graph" if has_graph else "exact scan",
                        "model": " ".join(x for x in (m_buf.raw[:cfg_v.model_len].decode("utf-8", "replace"),
                                                      mv_buf.raw[:cfg_v.model_version_len].decode("utf-8", "replace"))
                                          if x)}
                stores.append(Store(name, Model.VECTOR, tot, meta=meta))
            elif model_val == self.b_types["CDB_MODEL_TIME_SERIES"]:
                ts_summary = self._get_ts_store_summary(sid, info)
                stores.append(Store(name, Model.TIMESERIES, ts_summary["total_points"], meta=ts_summary["meta"]))
            elif model_val == self.b_types["CDB_MODEL_LINK"]:
                if not defined_link_types:
                    stores.append(Store(name, Model.LINK, sum(link_counts.values())))

        stores.extend(defined_link_types)
        return stores

    # ---- DDL helpers
    def create_table(self, name: str, columns: list[tuple[str, str, bool, bool]]) -> int:
        c = self.clib.lib
        ct = self.ctypes
        col_arr = (self.b_types["cdb_table_column"] * len(columns))()
        pk_indices = []
        name_bytes_list = []
        for i, (cname, ctype, not_null, is_pk) in enumerate(columns):
            nb = cname.encode("utf-8")
            name_bytes_list.append(nb)
            col_arr[i].name = ct.cast(nb, ct.c_char_p)
            col_arr[i].name_len = len(nb)
            col_arr[i].not_null = 1 if not_null or is_pk else 0
            t_upper = ctype.upper()
            if t_upper in ("INT", "INTEGER"):
                col_arr[i].type = self.b_types["CDB_TYPE_INT"]
            elif t_upper in ("REAL", "FLOAT", "DOUBLE"):
                col_arr[i].type = self.b_types["CDB_TYPE_F64"]
            elif t_upper in ("BOOL", "BOOLEAN"):
                col_arr[i].type = self.b_types["CDB_TYPE_BOOL"]
            elif t_upper == "TIMESTAMP":
                col_arr[i].type = self.b_types["CDB_TYPE_TIMESTAMP"]
            else:
                col_arr[i].type = self.b_types["CDB_TYPE_STRING"]
            if is_pk:
                pk_indices.append(i)

        pk_arr = (ct.c_uint32 * len(pk_indices))(*pk_indices) if pk_indices else None
        tdef = self.b_types["cdb_table_def"]()
        tdef.columns = col_arr
        tdef.ncolumns = len(columns)
        tdef.primary_key = pk_arr
        tdef.npk = len(pk_indices)

        sid = ct.c_uint64()
        tn_b = name.encode("utf-8")
        s = c.cdb_table_create(ct.byref(self._db), tn_b, len(tn_b), ct.byref(tdef), ct.byref(sid))
        if s != self.b_types["CDB_OK"]:
            raise BackendError(f"Table creation failed: {self.b_types['status_str'](self.clib, s)}")
        return sid.value

    def create_pool(self, name: str) -> int:
        c = self.clib.lib
        ct = self.ctypes
        sid = ct.c_uint64()
        nb = name.encode("utf-8")
        s = c.cdb_db_create_store(ct.byref(self._db), nb, len(nb), self.b_types["CDB_MODEL_DOCUMENT"], ct.byref(sid))
        if s != self.b_types["CDB_OK"]:
            raise BackendError(f"Pool creation failed: {self.b_types['status_str'](self.clib, s)}")
        return sid.value

    def create_collection(self, name: str, dims: int = 384) -> int:
        c = self.clib.lib
        ct = self.ctypes
        cfg = self.b_types["cdb_vec_config"]()
        cfg.elem = self.b_types["CDB_VEC_F32"]
        cfg.dims = dims
        cfg.measure = self.b_types["CDB_VEC_COSINE"]
        m_name = b"default"
        cfg.model = ct.cast(m_name, ct.c_char_p)
        cfg.model_len = len(m_name)
        v_name = b"1.0"
        cfg.model_version = ct.cast(v_name, ct.c_char_p)
        cfg.model_version_len = len(v_name)
        sid = ct.c_uint64()
        nb = name.encode("utf-8")
        s = c.cdb_vec_create(ct.byref(self._db), nb, len(nb), ct.byref(cfg), ct.byref(sid))
        if s != self.b_types["CDB_OK"]:
            raise BackendError(f"Collection creation failed: {self.b_types['status_str'](self.clib, s)}")
        return sid.value

    def create_link_type(self, name: str, cardinality: str = "n:m", enforced: bool = False, on_delete: str = "cascade") -> int:
        c = self.clib.lib
        ct = self.ctypes
        card = self.b_types["CDB_CARD_N_M"]
        if cardinality == "1:1":
            card = self.b_types["CDB_CARD_1_1"]
        elif cardinality in ("1:n", "1:N"):
            card = self.b_types["CDB_CARD_1_N"]
        on_del = 1 if on_delete.lower() in ("refuse", "restrict") else 0
        tid = ct.c_uint64()
        nb = name.encode("utf-8")
        s = c.cdb_link_define_type(ct.byref(self._db), nb, len(nb), card, 1 if enforced else 0, on_del, ct.byref(tid))
        if s != self.b_types["CDB_OK"]:
            raise BackendError(f"Link type creation failed: {self.b_types['status_str'](self.clib, s)}")
        return tid.value

    def create_ts_store(self, name: str, late_mode: int = 0, lateness: int = 0) -> int:
        c = self.clib.lib
        ct = self.ctypes
        sid = ct.c_uint64()
        nb = name.encode("utf-8")
        s = c.cdb_ts_create(ct.byref(self._db), nb, len(nb), late_mode, lateness, ct.byref(sid))
        if s != self.b_types["CDB_OK"]:
            raise BackendError(f"TS store creation failed: {self.b_types['status_str'](self.clib, s)}")
        return sid.value

    # ---- tables
    def table_schema(self, name: str) -> Result:
        sid = self._find_store_id(name)
        c = self.clib.lib
        ct = self.ctypes
        desc_cols = (self.b_types["cdb_table_column_info"] * 64)()
        n_cols = ct.c_uint32()
        ver = ct.c_uint32()
        keep = ct.c_uint32()
        s = c.cdb_table_describe(ct.byref(self._db), sid, desc_cols, 64,
                                 ct.byref(n_cols), ct.byref(ver), ct.byref(keep))
        if s != self.b_types["CDB_OK"]:
            raise BackendError(f"Table describe failed: {self.b_types['status_str'](self.clib, s)}")

        cat_ptr = self.b_types["get_catalog_ptr"](ct.byref(self._db))
        rows = []
        for i in range(n_cols.value):
            ci = desc_cols[i]
            nb = ct.create_string_buffer(64)
            nl = ct.c_size_t()
            c.cdb_catalog_field_name(cat_ptr, sid, ci.field_id, nb, 64, ct.byref(nl))
            cname = nb.value.decode("utf-8")
            t_str = "INT" if ci.type == self.b_types["CDB_TYPE_INT"] else (
                "TEXT" if ci.type == self.b_types["CDB_TYPE_STRING"] else (
                    "REAL" if ci.type in (self.b_types["CDB_TYPE_F64"], self.b_types["CDB_TYPE_F32"]) else (
                        "BOOL" if ci.type == self.b_types["CDB_TYPE_BOOL"] else (
                            "TIMESTAMP" if ci.type == self.b_types["CDB_TYPE_TIMESTAMP"] else "TEXT"
                        )
                    )
                )
            )
            rows.append((
                cname,
                t_str,
                "NO" if ci.not_null else "YES",
                None,
                "PRIMARY KEY" if ci.primary else ""
            ))
        return Result(["column", "type", "nullable", "default", "constraints"], rows)

    def table_indexes(self, name: str) -> Result:
        schema = self.table_schema(name)
        pk_col = next((r[0] for r in schema.rows if "PRIMARY" in r[4]), "id")
        rows = [(f"{name}_pkey", pk_col, "btree", True)]
        return Result(["index", "columns", "kind", "unique"], rows)

    def table_rows(self, name: str, offset: int, limit: int, filter_text: str = "") -> Result:
        t0 = time.perf_counter()
        sid = self._find_store_id(name)
        c = self.clib.lib
        ct = self.ctypes

        schema = self.table_schema(name)
        cols = [r[0] for r in schema.rows]

        # Scan all row entries
        cur = self.b_types["cdb_table_cursor"]()
        ct.memset(ct.byref(cur), 0, ct.sizeof(cur))
        entries = (ct.c_uint64 * 1024)()
        n = ct.c_size_t()

        all_entries = []
        while True:
            s = c.cdb_table_scan(ct.byref(self._db), sid, self.b_types["CDB_TABLE_NO_INDEX"],
                                 None, 0, None, 0, ct.byref(cur), entries, 1024, ct.byref(n))
            if s != self.b_types["CDB_OK"] or n.value == 0:
                break
            for i in range(n.value):
                all_entries.append(entries[i])

        all_rows = []
        read_buf = ct.create_string_buffer(65536)
        read_len = ct.c_size_t()

        # Map field IDs to column positions
        cat_ptr = self.b_types["get_catalog_ptr"](ct.byref(self._db))
        f_map = {}
        for cname in cols:
            fid = ct.c_uint64()
            cn_b = cname.encode("utf-8")
            s = c.cdb_table_column_id(ct.byref(self._db), sid, cn_b, len(cn_b), ct.byref(fid))
            if s == self.b_types["CDB_OK"]:
                f_map[fid.value] = cname

        for eid in all_entries:
            s_get = c.cdb_table_get(ct.byref(self._db), sid, eid, read_buf, 65536, ct.byref(read_len))
            if s_get != self.b_types["CDB_OK"]:
                continue
            val = self.b_types["cdb_value"]()
            c.cdb_value_read(ct.cast(read_buf, ct.POINTER(ct.c_uint8)), read_len.value,
                             self.b_types["CDB_ORDER_LITTLE"], ct.byref(val))
            obj_dict = self.b_types["decode_value"](val, self.clib) or {}
            row_vals = []
            for col in cols:
                # Find matching field id
                v = None
                for fid, val_item in obj_dict.items():
                    if f_map.get(fid) == col:
                        v = val_item
                        break
                if v is None and col == cols[0]:
                    v = eid
                row_vals.append(v)
            all_rows.append(tuple(row_vals))

        if filter_text:
            q = filter_text.lower()
            all_rows = [r for r in all_rows if any(q in str(v).lower() for v in r if v is not None)]

        paged = all_rows[offset:offset + limit]
        elapsed = (time.perf_counter() - t0) * 1000
        return Result(cols, paged, len(all_rows), elapsed)

    def table_insert(self, name: str, values: dict) -> object:
        sid = self._find_store_id(name)
        c = self.clib.lib
        ct = self.ctypes

        schema = self.table_schema(name)
        fields = []
        for r in schema.rows:
            cname = r[0]
            val = values.get(cname)
            if val is not None:
                fid = ct.c_uint64()
                cn_b = cname.encode("utf-8")
                c.cdb_table_column_id(ct.byref(self._db), sid, cn_b, len(cn_b), ct.byref(fid))
                fields.append((fid.value, r[1], val))

        fields.sort(key=lambda item: item[0])

        w = self.b_types["cdb_value_writer"]()
        row_buf = ct.create_string_buffer(65536)
        c.cdb_value_writer_init(ct.byref(w), ct.cast(row_buf, ct.POINTER(ct.c_uint8)), 65536,
                                self.b_types["CDB_ORDER_LITTLE"], self.b_types["CDB_DECIMAL_COMPACT"])
        c.cdb_value_begin_object(ct.byref(w))
        for fid, col_type, val in fields:
            c.cdb_value_field(ct.byref(w), fid)
            if col_type == "INT":
                c.cdb_value_write_int(ct.byref(w), int(val))
            elif col_type == "REAL":
                c.cdb_value_write_f64(ct.byref(w), float(val))
            elif col_type == "BOOL":
                c.cdb_value_write_bool(ct.byref(w), 1 if val else 0)
            else:
                s_bytes = str(val).encode("utf-8")
                c.cdb_value_write_string(ct.byref(w), s_bytes, len(s_bytes))
        c.cdb_value_end_object(ct.byref(w))
        row_len = c.cdb_value_writer_len(ct.byref(w))

        c.cdb_tx_begin(ct.byref(self._db), self.b_types["CDB_DURABILITY_DEFAULT"])
        new_entry = ct.c_uint64()
        s = c.cdb_table_insert(ct.byref(self._db), sid, row_buf, row_len, ct.byref(new_entry))
        if s != self.b_types["CDB_OK"]:
            c.cdb_tx_abort(ct.byref(self._db))
            raise BackendError(f"Insert failed: {self.b_types['status_str'](self.clib, s)}")
        c.cdb_tx_commit(ct.byref(self._db))
        return new_entry.value

    def table_update(self, name: str, pk, column: str, value) -> None:
        sid = self._find_store_id(name)
        schema = self.table_schema(name)
        col_info = next((r for r in schema.rows if r[0] == column), None)
        if not col_info:
            raise BackendError(f"No column '{column}' in '{name}'")
        if "PRIMARY" in col_info[4]:
            raise BackendError("Primary keys are immutable")

        c = self.clib.lib
        ct = self.ctypes
        fid = ct.c_uint64()
        cn_b = column.encode("utf-8")
        c.cdb_table_column_id(ct.byref(self._db), sid, cn_b, len(cn_b), ct.byref(fid))

        w = self.b_types["cdb_value_writer"]()
        chg_buf = ct.create_string_buffer(4096)
        c.cdb_value_writer_init(ct.byref(w), ct.cast(chg_buf, ct.POINTER(ct.c_uint8)), 4096,
                                self.b_types["CDB_ORDER_LITTLE"], self.b_types["CDB_DECIMAL_COMPACT"])
        c.cdb_value_begin_object(ct.byref(w))
        c.cdb_value_field(ct.byref(w), fid.value)
        if value is None:
            c.cdb_value_write_null(ct.byref(w))
        elif col_info[1] == "INT":
            c.cdb_value_write_int(ct.byref(w), int(value))
        elif col_info[1] == "REAL":
            c.cdb_value_write_f64(ct.byref(w), float(value))
        elif col_info[1] == "BOOL":
            c.cdb_value_write_bool(ct.byref(w), 1 if value else 0)
        else:
            s_bytes = str(value).encode("utf-8")
            c.cdb_value_write_string(ct.byref(w), s_bytes, len(s_bytes))
        c.cdb_value_end_object(ct.byref(w))
        chg_len = c.cdb_value_writer_len(ct.byref(w))

        c.cdb_tx_begin(ct.byref(self._db), self.b_types["CDB_DURABILITY_DEFAULT"])
        s = c.cdb_table_update(ct.byref(self._db), sid, int(pk), chg_buf, chg_len)
        if s != self.b_types["CDB_OK"]:
            c.cdb_tx_abort(ct.byref(self._db))
            raise BackendError(f"Update failed: {self.b_types['status_str'](self.clib, s)}")
        c.cdb_tx_commit(ct.byref(self._db))

    def table_delete(self, name: str, pks: list) -> int:
        sid = self._find_store_id(name)
        c = self.clib.lib
        ct = self.ctypes
        c.cdb_tx_begin(ct.byref(self._db), self.b_types["CDB_DURABILITY_DEFAULT"])
        deleted = 0
        for pk in pks:
            s = c.cdb_table_delete(ct.byref(self._db), sid, int(pk))
            if s == self.b_types["CDB_OK"]:
                deleted += 1
        c.cdb_tx_commit(ct.byref(self._db))
        return deleted

    # ---- documents
    def _parse_doc_id(self, doc_id: str) -> int:
        if isinstance(doc_id, int):
            return doc_id
        m = re.search(r"\d+", str(doc_id))
        if m:
            return int(m.group())
        raise BackendError(f"Invalid document id: {doc_id}")

    def doc_list(self, pool: str, limit: int = 200) -> list[DocHit]:
        sid = self._find_store_id(pool)
        c = self.clib.lib
        ct = self.ctypes

        hits = []
        after = ct.c_uint64(0)
        entry = ct.c_uint64(0)
        done = ct.c_int(0)
        dummy_body = ct.create_string_buffer(self.DOC_MAX)
        dummy_len = ct.c_size_t()
        sum_buf = ct.create_string_buffer(512)
        sum_len = ct.c_size_t()
        info = self.b_types["cdb_doc_info"]()

        while len(hits) < limit:
            s = c.cdb_doc_next_live(ct.byref(self._db), sid, after.value, ct.byref(entry), ct.byref(done))
            if s != self.b_types["CDB_OK"] or done.value:
                break
            # Get summary
            c.cdb_doc_get(ct.byref(self._db), sid, entry.value, 0, dummy_body, self.DOC_MAX, ct.byref(dummy_len),
                          sum_buf, 512, ct.byref(sum_len), ct.byref(info))
            title = ct.string_at(sum_buf, sum_len.value).decode("utf-8", errors="replace") if sum_len.value else f"Document {entry.value}"
            hits.append(DocHit(f"doc-{entry.value:05d}", title, max(1, int(info.revision))))
            after.value = entry.value

        return hits

    def fts_search(self, pool: str, query: str, limit: int = 50) -> list[DocHit]:
        sid = self._find_store_id(pool)
        c = self.clib.lib
        ct = self.ctypes

        raw_hits = (self.b_types["cdb_fts_hit"] * limit)()
        n_hits = ct.c_size_t()
        qb = query.encode("utf-8")
        s = c.cdb_doc_search(ct.byref(self._db), sid, qb, len(qb), self.b_types["CDB_FTS_ANY"],
                             raw_hits, limit, ct.byref(n_hits))
        if s != self.b_types["CDB_OK"]:
            raise BackendError(f"Search failed: {self.b_types['status_str'](self.clib, s)}")

        results = []
        dummy_body = ct.create_string_buffer(self.DOC_MAX)
        dummy_len = ct.c_size_t()
        sum_buf = ct.create_string_buffer(512)
        sum_len = ct.c_size_t()
        info = self.b_types["cdb_doc_info"]()

        for i in range(n_hits.value):
            hit = raw_hits[i]
            c.cdb_doc_get(ct.byref(self._db), sid, hit.entry, 0, dummy_body, self.DOC_MAX, ct.byref(dummy_len),
                          sum_buf, 512, ct.byref(sum_len), ct.byref(info))
            title = ct.string_at(sum_buf, sum_len.value).decode("utf-8", errors="replace") if sum_len.value else f"Document {hit.entry}"
            results.append(DocHit(f"doc-{hit.entry:05d}", title, max(1, int(info.revision)), round(float(hit.score), 3)))
        return results

    def doc_revisions(self, pool: str, doc_id: str) -> list[int]:
        sid = self._find_store_id(pool)
        eid = self._parse_doc_id(doc_id)
        c = self.clib.lib
        ct = self.ctypes

        rev = ct.c_uint64()
        live = ct.c_int()
        s = c.cdb_doc_head(ct.byref(self._db), sid, eid, ct.byref(rev), ct.byref(live))
        if s != self.b_types["CDB_OK"] or not live.value:
            raise BackendError(f"Document {doc_id} not found or deleted")
        return list(range(1, max(1, int(rev.value)) + 1))

    def _encode_doc_body(self, doc: dict) -> tuple[ctypes.Array, int]:
        c = self.clib.lib
        ct = self.ctypes
        clean_doc = {k: v for k, v in doc.items() if not k.startswith("_")}
        json_bytes = json.dumps(clean_doc).encode("utf-8")
        buf = ct.create_string_buffer(len(json_bytes) + 256)
        w = self.b_types["cdb_value_writer"]()
        c.cdb_value_writer_init(ct.byref(w), ct.cast(buf, ct.POINTER(ct.c_uint8)), len(buf),
                                self.b_types["CDB_ORDER_LITTLE"], self.b_types["CDB_DECIMAL_COMPACT"])
        c.cdb_value_begin_object(ct.byref(w))
        c.cdb_value_field(ct.byref(w), 1)
        c.cdb_value_write_string(ct.byref(w), json_bytes, len(json_bytes))
        c.cdb_value_end_object(ct.byref(w))
        vlen = c.cdb_value_writer_len(ct.byref(w))
        return buf, vlen

    def doc_get(self, pool: str, doc_id: str, rev: int | None = None) -> dict:
        sid = self._find_store_id(pool)
        eid = self._parse_doc_id(doc_id)
        c = self.clib.lib
        ct = self.ctypes

        body_buf = ct.create_string_buffer(self.DOC_MAX)
        body_len = ct.c_size_t()
        info = self.b_types["cdb_doc_info"]()
        target_rev = rev or 0

        s = c.cdb_doc_get(ct.byref(self._db), sid, eid, target_rev,
                          body_buf, self.DOC_MAX, ct.byref(body_len),
                          None, 0, None, ct.byref(info))
        if s != self.b_types["CDB_OK"]:
            raise BackendError(f"Failed to fetch document: {self.b_types['status_str'](self.clib, s)}")

        val = self.b_types["cdb_value"]()
        s_read = c.cdb_value_read(ct.cast(body_buf, ct.POINTER(ct.c_uint8)), body_len.value,
                                  self.b_types["CDB_ORDER_LITTLE"], ct.byref(val))
        if s_read == self.b_types["CDB_OK"]:
            decoded = self.b_types["decode_value"](val, self.clib)
            # Documents the studio writes keep JSON text in an unnamed field 1;
            # a field with a name in the dictionary is real data
            if (isinstance(decoded, dict) and 1 in decoded and isinstance(decoded[1], str)
                    and self._field_name(sid, 1) is None):
                try:
                    doc = json.loads(decoded[1])
                except Exception:
                    doc = {"content": decoded[1]}
            elif isinstance(decoded, dict):
                doc = self._named(sid, decoded)
            else:
                doc = {"value": decoded}
        else:
            body_str = ct.string_at(body_buf, body_len.value).decode("utf-8", errors="replace")
            try:
                doc = json.loads(body_str)
            except Exception:
                doc = {"raw": body_str}

        doc["_id"] = doc_id
        doc["_rev"] = info.revision
        return doc

    def doc_put(self, pool: str, doc_id: str, doc: dict) -> int:
        if not isinstance(doc, dict):
            raise BackendError("A document must be a JSON object")
        sid = self._find_store_id(pool)
        eid = self._parse_doc_id(doc_id)
        c = self.clib.lib
        ct = self.ctypes

        title = str(doc.get("title", f"Document {eid}")).encode("utf-8")
        body_buf, body_len = self._encode_doc_body(doc)

        c.cdb_tx_begin(ct.byref(self._db), self.b_types["CDB_DURABILITY_DEFAULT"])
        s = c.cdb_doc_replace(ct.byref(self._db), sid, eid, title, len(title), body_buf, body_len)
        if s != self.b_types["CDB_OK"]:
            c.cdb_tx_abort(ct.byref(self._db))
            raise BackendError(f"Doc update failed: {self.b_types['status_str'](self.clib, s)}")
        c.cdb_tx_commit(ct.byref(self._db))

        head_rev = ct.c_uint64()
        live = ct.c_int()
        c.cdb_doc_head(ct.byref(self._db), sid, eid, ct.byref(head_rev), ct.byref(live))
        return head_rev.value

    def doc_create(self, pool: str, doc: dict) -> str:
        if not isinstance(doc, dict):
            raise BackendError("A document must be a JSON object")
        sid = self._find_store_id(pool)
        c = self.clib.lib
        ct = self.ctypes

        title = str(doc.get("title", "Untitled")).encode("utf-8")
        body_buf, body_len = self._encode_doc_body(doc)

        new_entry = ct.c_uint64()
        c.cdb_tx_begin(ct.byref(self._db), self.b_types["CDB_DURABILITY_DEFAULT"])
        s = c.cdb_doc_insert(ct.byref(self._db), sid, title, len(title), body_buf, body_len, ct.byref(new_entry))
        if s != self.b_types["CDB_OK"]:
            c.cdb_tx_abort(ct.byref(self._db))
            raise BackendError(f"Doc creation failed: {self.b_types['status_str'](self.clib, s)}")
        c.cdb_tx_commit(ct.byref(self._db))
        return f"doc-{new_entry.value:05d}"

    def doc_delete(self, pool: str, doc_id: str) -> None:
        sid = self._find_store_id(pool)
        eid = self._parse_doc_id(doc_id)
        c = self.clib.lib
        ct = self.ctypes

        c.cdb_tx_begin(ct.byref(self._db), self.b_types["CDB_DURABILITY_DEFAULT"])
        s = c.cdb_doc_delete(ct.byref(self._db), sid, eid)
        if s != self.b_types["CDB_OK"]:
            c.cdb_tx_abort(ct.byref(self._db))
            raise BackendError(f"Doc deletion failed: {self.b_types['status_str'](self.clib, s)}")
        c.cdb_tx_commit(ct.byref(self._db))

    # ---- vectors
    def _parse_vec_id(self, vec_id: str) -> int:
        if isinstance(vec_id, int):
            return vec_id
        m = re.search(r"\d+", str(vec_id))
        if m:
            return int(m.group())
        raise BackendError(f"Invalid vector id: {vec_id}")

    def vector_get(self, coll: str, vec_id: str) -> list[float]:
        sid = self._find_store_id(coll)
        eid = self._parse_vec_id(vec_id)
        c = self.clib.lib
        ct = self.ctypes

        buf = ct.create_string_buffer(4096)
        out_len = ct.c_size_t()
        src_store = ct.c_uint64()
        src_entry = ct.c_uint64()
        s = c.cdb_vec_get(ct.byref(self._db), sid, eid, buf, 4096, ct.byref(out_len),
                          ct.byref(src_store), ct.byref(src_entry))
        if s != self.b_types["CDB_OK"]:
            raise BackendError(f"Vector not found: {vec_id}")

        val = self.b_types["cdb_value"]()
        c.cdb_value_read(ct.cast(buf, ct.POINTER(ct.c_uint8)), out_len.value,
                         self.b_types["CDB_ORDER_LITTLE"], ct.byref(val))
        return self.b_types["decode_value"](val, self.clib) or []

    def vector_search(self, coll: str, query: str, k: int, approximate: bool = False) -> list[tuple[str, float]]:
        sid = self._find_store_id(coll)
        c = self.clib.lib
        ct = self.ctypes

        q = query.strip()
        if q.startswith("["):
            try:
                vec = [float(x) for x in json.loads(q)]
            except Exception:
                raise BackendError("Vector literal must be a JSON array of floats")
        else:
            vec = self.vector_get(coll, q)

        # Encode vector into writer
        w = self.b_types["cdb_value_writer"]()
        v_buf = ct.create_string_buffer(4096)
        c.cdb_value_writer_init(ct.byref(w), ct.cast(v_buf, ct.POINTER(ct.c_uint8)), 4096,
                                self.b_types["CDB_ORDER_LITTLE"], self.b_types["CDB_DECIMAL_COMPACT"])
        arr_type = ct.c_float * len(vec)
        arr = arr_type(*vec)
        c.cdb_value_write_vector(ct.byref(w), self.b_types["CDB_VEC_F32"], len(vec), ct.cast(arr, ct.c_void_p))
        v_len = c.cdb_value_writer_len(ct.byref(w))

        hits = (self.b_types["cdb_vec_hit"] * k)()
        n_hits = ct.c_size_t()
        mode = self.b_types["CDB_VEC_APPROX"] if approximate else self.b_types["CDB_VEC_EXACT"]
        s = c.cdb_vec_search_mode(ct.byref(self._db), sid, v_buf, v_len, 0, mode, hits, k, ct.byref(n_hits))
        if s != self.b_types["CDB_OK"]:
            raise BackendError(f"Vector search failed: {self.b_types['status_str'](self.clib, s)}")

        return [(f"vec-{hits[i].entry:05d}", round(float(hits[i].distance), 4)) for i in range(n_hits.value)]

    def vector_graph(self, coll: str, limit: int = 220) -> VectorGraph:
        sid = self._find_store_id(coll)
        c = self.clib.lib
        ct = self.ctypes

        ids = []
        vecs = []
        for i in range(1, limit + 1):
            try:
                v = self.vector_get(coll, f"vec-{i:05d}")
                ids.append(f"vec-{i:05d}")
                vecs.append(v)
            except BackendError:
                break

        edges = set()
        for idx, vid in enumerate(ids):
            eid = self._parse_vec_id(vid)
            nhits = (self.b_types["cdb_vec_hit"] * 16)()
            n = ct.c_size_t()
            fresh = ct.c_int()
            s = c.cdb_vec_neighbours(ct.byref(self._db), sid, eid, nhits, 16, ct.byref(n), ct.byref(fresh))
            if s == self.b_types["CDB_OK"] and n.value > 0:
                for k in range(n.value):
                    target_id = f"vec-{nhits[k].entry:05d}"
                    if target_id in ids:
                        j = ids.index(target_id)
                        edges.add((min(idx, j), max(idx, j)))

        # Fallback to nearest neighbors if graph wasn't constructed
        if not edges and vecs:
            for i, a in enumerate(vecs):
                near = sorted(range(len(vecs)), key=lambda j: _cos_dist(a, vecs[j]) if j != i else 9)[:3]
                edges.update((min(i, j), max(i, j)) for j in near)

        labels = [self.entry_label(sid, self._parse_vec_id(v)) for v in ids]
        return VectorGraph(ids, vecs, sorted(edges), labels)

    def vector_put(self, coll: str, vec_id: str, vec: list[float]) -> None:
        sid = self._find_store_id(coll)
        eid = self._parse_vec_id(vec_id)
        c = self.clib.lib
        ct = self.ctypes

        w = self.b_types["cdb_value_writer"]()
        v_buf = ct.create_string_buffer(4096)
        c.cdb_value_writer_init(ct.byref(w), ct.cast(v_buf, ct.POINTER(ct.c_uint8)), 4096,
                                self.b_types["CDB_ORDER_LITTLE"], self.b_types["CDB_DECIMAL_COMPACT"])
        arr_type = ct.c_float * len(vec)
        arr = arr_type(*vec)
        c.cdb_value_write_vector(ct.byref(w), self.b_types["CDB_VEC_F32"], len(vec), ct.cast(arr, ct.c_void_p))
        v_len = c.cdb_value_writer_len(ct.byref(w))

        c.cdb_tx_begin(ct.byref(self._db), self.b_types["CDB_DURABILITY_DEFAULT"])
        s = c.cdb_vec_replace(ct.byref(self._db), sid, eid, v_buf, v_len)
        if s != self.b_types["CDB_OK"]:
            new_eid = ct.c_uint64()
            s_ins = c.cdb_vec_insert(ct.byref(self._db), sid, v_buf, v_len, 0, 0, ct.byref(new_eid))
            if s_ins != self.b_types["CDB_OK"]:
                c.cdb_tx_abort(ct.byref(self._db))
                raise BackendError(f"Vector put failed: {self.b_types['status_str'](self.clib, s_ins)}")
        c.cdb_tx_commit(ct.byref(self._db))
        self._vec_count_cache.pop(sid, None)

    def vector_delete(self, coll: str, vec_id: str) -> None:
        sid = self._find_store_id(coll)
        eid = self._parse_vec_id(vec_id)
        c = self.clib.lib
        ct = self.ctypes

        c.cdb_tx_begin(ct.byref(self._db), self.b_types["CDB_DURABILITY_DEFAULT"])
        s = c.cdb_vec_delete(ct.byref(self._db), sid, eid)
        if s != self.b_types["CDB_OK"]:
            c.cdb_tx_abort(ct.byref(self._db))
            raise BackendError(f"Vector delete failed: {self.b_types['status_str'](self.clib, s)}")
        c.cdb_tx_commit(ct.byref(self._db))
        self._vec_count_cache.pop(sid, None)

    # ---- links
    def _parse_ref(self, target: str) -> cdb_ref:
        ct = self.ctypes
        ref = self.b_types["cdb_ref"]()
        m = re.match(r"([A-Za-z0-9_]+)[-_](\d+)", target)
        if m:
            store_name, eid = m.group(1), int(m.group(2))
            try:
                ref.store = self._find_store_id(store_name)
            except BackendError:
                ref.store = 1
            ref.entry = eid
        else:
            ref.store = 1
            ref.entry = int(re.search(r"\d+", target).group() if re.search(r"\d+", target) else 1)
        ref.ts = 0
        ref.flags = 0
        return ref

    def link_edges(self, name: str, limit: int | None = 500) -> Result:
        t0 = time.perf_counter()
        cached = self._link_cache.get(name)
        if cached is not None:
            return Result(["source", "target", "created", "weight"], cached[:limit], len(cached), (time.perf_counter() - t0) * 1000)

        c = self.clib.lib
        ct = self.ctypes

        type_id = ct.c_uint64()
        found = ct.c_int()
        nb = name.encode("utf-8")
        s = c.cdb_link_find_type(ct.byref(self._db), nb, len(nb), ct.byref(type_id), ct.byref(found))
        if s != self.b_types["CDB_OK"] or not found.value:
            return Result(["source", "target", "created", "weight"], [], 0, 0.0)

        store_names = self._store_names_map()
        links_info = self._get_links_store_info()
        if not links_info or not links_info.head_root:
            return Result(["source", "target", "created", "weight"], [], 0, 0.0)

        pager_ptr = self.b_types["get_pager_ptr"](ct.byref(self._db))
        fwd_ix = self.b_types["cdb_index"]()
        if c.cdb_index_open(ct.byref(fwd_ix), pager_ptr, links_info.head_root, ct.byref(self._cfg.catalog_work)) != self.b_types["CDB_OK"]:
            return Result(["source", "target", "created", "weight"], [], 0, 0.0)

        cur = self.b_types["cdb_index_cursor"]()
        page_buf = ct.create_string_buffer(self.PAGE_SIZE)
        if c.cdb_index_seek(ct.byref(cur), ct.byref(fwd_ix), page_buf, None, 0) != self.b_types["CDB_OK"]:
            return Result(["source", "target", "created", "weight"], [], 0, 0.0)

        k_buf = ct.create_string_buffer(512)
        v_buf = ct.create_string_buffer(128)
        klen = ct.c_size_t()
        vlen = ct.c_size_t()
        done = ct.c_int(0)
        rows = []
        refs = []  # (source store, entry, target store, entry) per row
        import struct

        while c.cdb_index_next(ct.byref(cur), k_buf, 512, ct.byref(klen), v_buf, 128, ct.byref(vlen), ct.byref(done)) == self.b_types["CDB_OK"] and not done.value:
            raw_k = k_buf.raw[:klen.value]
            raw_v = v_buf.raw[:vlen.value]
            if klen.value == 40:
                s_store, s_entry, l_type, t_store, t_entry = struct.unpack(">QQQQQ", raw_k[:40])
            elif klen.value == 58:
                s_store, s_entry, s_fl, s_ts, l_type, t_store, t_entry, t_fl, t_ts = struct.unpack(">QQBQ Q QQBQ", raw_k[:58])
            else:
                continue

            if l_type != type_id.value:
                continue

            w = struct.unpack(">f", raw_v[:4])[0]
            l_time = struct.unpack(">Q", raw_v[4:12])[0]
            date_str = time.strftime("%Y-%m-%d", time.gmtime(l_time / 1e9)) if l_time > 0 else "2026-01-01"
            s_pfx = store_names.get(s_store, f"store{s_store}")
            t_pfx = store_names.get(t_store, f"store{t_store}")
            rows.append((f"{s_pfx}-{s_entry:05d}", f"{t_pfx}-{t_entry:05d}", date_str, round(w, 3)))
            refs.append((s_store, s_entry, t_store, t_entry))

        # What each end is, for the rows shown (labels read the entries)
        shown = [r + (self.entry_label(ref[0], ref[1]), self.entry_label(ref[2], ref[3]))
                 for r, ref in zip(rows[:limit], refs[:limit])]
        return Result(["source", "target", "created", "weight", "source_label", "target_label"], shown, len(rows),
                      (time.perf_counter() - t0) * 1000)

    def link_add(self, name: str, source: str, target: str, weight: float = 1.0) -> None:
        c = self.clib.lib
        ct = self.ctypes

        type_id = ct.c_uint64()
        found = ct.c_int()
        nb = name.encode("utf-8")
        s = c.cdb_link_find_type(ct.byref(self._db), nb, len(nb), ct.byref(type_id), ct.byref(found))
        if s != self.b_types["CDB_OK"] or not found.value:
            # Auto-create link type
            c.cdb_link_define_type(ct.byref(self._db), nb, len(nb), self.b_types["CDB_CARD_N_M"], 0,
                                  0, ct.byref(type_id))

        s_ref = self._parse_ref(source)
        t_ref = self._parse_ref(target)

        c.cdb_tx_begin(ct.byref(self._db), self.b_types["CDB_DURABILITY_DEFAULT"])
        now_ns = int(time.time() * 1e9)
        s_add = c.cdb_link_add(ct.byref(self._db), type_id.value, s_ref, t_ref, float(weight), now_ns)
        if s_add != self.b_types["CDB_OK"]:
            c.cdb_tx_abort(ct.byref(self._db))
            raise BackendError(f"Link add failed: {self.b_types['status_str'](self.clib, s_add)}")
        c.cdb_tx_commit(ct.byref(self._db))
        date_str = time.strftime("%Y-%m-%d")
        self._link_cache.setdefault(name, []).append((source, target, date_str, round(float(weight), 3)))

    def link_delete(self, name: str, pairs: list[tuple[str, str]]) -> int:
        c = self.clib.lib
        ct = self.ctypes

        type_id = ct.c_uint64()
        found = ct.c_int()
        nb = name.encode("utf-8")
        s = c.cdb_link_find_type(ct.byref(self._db), nb, len(nb), ct.byref(type_id), ct.byref(found))
        if s != self.b_types["CDB_OK"] or not found.value:
            return 0

        c.cdb_tx_begin(ct.byref(self._db), self.b_types["CDB_DURABILITY_DEFAULT"])
        deleted = 0
        for src, tgt in pairs:
            s_ref = self._parse_ref(src)
            t_ref = self._parse_ref(tgt)
            s_rem = c.cdb_link_remove(ct.byref(self._db), type_id.value, s_ref, t_ref)
            if s_rem == self.b_types["CDB_OK"]:
                deleted += 1
        c.cdb_tx_commit(ct.byref(self._db))
        if name in self._link_cache:
            drop = set(pairs)
            self._link_cache[name] = [r for r in self._link_cache[name] if (r[0], r[1]) not in drop]
        return deleted

    # ---- time series
    def ts_series_list(self, name: str) -> list[str]:
        sid = self._find_store_id(name)
        c = self.clib.lib
        ct = self.ctypes
        cat_ptr = self.b_types["get_catalog_ptr"](ct.byref(self._db))
        info = self.b_types["cdb_store_info"]()
        if c.cdb_catalog_get_store(cat_ptr, sid, ct.byref(info)) != self.b_types["CDB_OK"]:
            return []
        summary = self._get_ts_store_summary(sid, info)
        return summary["meta"].get("series", [])

    def ts_points(self, name: str, window_s: int, series: str | None = None) -> list[tuple[float, float]]:
        sid = self._find_store_id(name)
        c = self.clib.lib
        ct = self.ctypes

        cat_ptr = self.b_types["get_catalog_ptr"](ct.byref(self._db))
        info = self.b_types["cdb_store_info"]()
        c.cdb_catalog_get_store(cat_ptr, sid, ct.byref(info))
        summary = self._get_ts_store_summary(sid, info)
        series_map = summary["meta"].get("series_map", {})

        target_series_id = 1
        if series and series in series_map:
            target_series_id = series_map[series]
        elif series_map:
            first_name = summary["meta"]["series"][0]
            target_series_id = series_map[first_name]

        buf = ct.create_string_buffer(65536)
        rows = (self.b_types["cdb_ts_row"] * 1024)()
        n = ct.c_size_t()
        nxt = ct.c_int64()

        raw_points = []
        cur_from = 0
        to_ts = 0x7FFFFFFFFFFFFFFF

        while len(raw_points) < 5000:
            s = c.cdb_ts_range(ct.byref(self._db), sid, target_series_id, cur_from, to_ts,
                               ct.cast(buf, ct.POINTER(ct.c_uint8)), 65536,
                               rows, 1024, ct.byref(n), ct.byref(nxt))
            if s != self.b_types["CDB_OK"] or n.value == 0:
                break

            for i in range(n.value):
                r = rows[i]
                val = self.b_types["cdb_value"]()
                s_v = c.cdb_value_read(ct.cast(ct.addressof(buf) + r.off, ct.POINTER(ct.c_uint8)),
                                       r.len, self.b_types["CDB_ORDER_LITTLE"], ct.byref(val))
                num_val = 0.0
                if s_v == self.b_types["CDB_OK"]:
                    obj = self.b_types["decode_value"](val, self.clib)
                    if isinstance(obj, dict):
                        for v in obj.values():
                            if isinstance(v, (int, float)):
                                num_val = float(v)
                                break
                    elif isinstance(obj, (int, float)):
                        num_val = float(obj)
                raw_points.append((r.ts, num_val))

            if nxt.value <= cur_from:
                break
            cur_from = nxt.value

        if not raw_points:
            return []

        # Determine timestamp scale: nanoseconds (>1e16), microseconds (>1e13), milliseconds (>1e10), or seconds
        sample_ts = raw_points[-1][0]
        if sample_ts > 1e16:
            scale = 1e9
        elif sample_ts > 1e13:
            scale = 1e6
        elif sample_ts > 1e10:
            scale = 1e3
        else:
            scale = 1.0

        if window_s > 0:
            now = time.time()
            max_t_sec = raw_points[-1][0] / scale
            if now - max_t_sec > window_s:
                cutoff_sec = max_t_sec - window_s
            else:
                cutoff_sec = now - window_s
            raw_points = [p for p in raw_points if (p[0] / scale) >= cutoff_sec]

        pts = [(ts / scale, val) for ts, val in raw_points]
        return sorted(pts)

    def ts_append(self, name: str, t: float, value: float, series: str | None = None) -> None:
        sid = self._find_store_id(name)
        c = self.clib.lib
        ct = self.ctypes

        series_name = (series or "default").encode("utf-8")
        series_id = ct.c_uint64(1)
        created = ct.c_int(0)
        s_def = self.b_types["cdb_ts_def"]()
        s_def.precision = 2  # micros (CDB_TS_MICROS)
        s_def.window = 0
        s_def.nfields = 1
        s_def.fields[0].id = 1
        s_def.fields[0].type = self.b_types["CDB_TYPE_F64"]

        c.cdb_tx_begin(ct.byref(self._db), self.b_types["CDB_DURABILITY_DEFAULT"])
        c.cdb_ts_series(ct.byref(self._db), sid, series_name, len(series_name), None, 0, ct.byref(s_def), ct.byref(series_id), ct.byref(created))

        ts_micros = int(t * 1e6)

        w = self.b_types["cdb_value_writer"]()
        v_buf = ct.create_string_buffer(512)
        c.cdb_value_writer_init(ct.byref(w), ct.cast(v_buf, ct.POINTER(ct.c_uint8)), 512,
                                self.b_types["CDB_ORDER_LITTLE"], self.b_types["CDB_DECIMAL_COMPACT"])
        c.cdb_value_begin_object(ct.byref(w))
        c.cdb_value_field(ct.byref(w), 1)
        c.cdb_value_write_f64(ct.byref(w), float(value))
        c.cdb_value_end_object(ct.byref(w))
        v_len = c.cdb_value_writer_len(ct.byref(w))

        pt_in = self.b_types["cdb_ts_in"]()
        pt_in.ts = ts_micros
        pt_in.use_clock = 0
        pt_in.values = ct.cast(v_buf, ct.c_void_p)
        pt_in.len = v_len

        s = c.cdb_ts_append(ct.byref(self._db), sid, series_id.value, ct.byref(pt_in), 1)
        if s != self.b_types["CDB_OK"]:
            c.cdb_tx_abort(ct.byref(self._db))
            raise BackendError(f"TS append failed: {self.b_types['status_str'](self.clib, s)}")
        c.cdb_tx_commit(ct.byref(self._db))
        c.cdb_tx_commit(ct.byref(self._db))

    # ---- query & procedures
    def proc_list(self) -> list[Procedure]:
        return sorted(self._procs.values(), key=lambda p: p.name)

    def proc_save(self, proc: Procedure) -> None:
        self._procs[proc.name] = proc

    def proc_delete(self, name: str) -> None:
        if self._procs.pop(name, None) is None:
            raise BackendError(f"No procedure {name}")

    def execute(self, text: str) -> Result:
        t0 = time.perf_counter()
        c = self.clib.lib
        ct = self.ctypes

        # Check for procedure call
        m = re.match(r"\s*call\s+(\w+)(?:\s*\((.*?)\))?", text, re.I)
        if m:
            p_name = m.group(1)
            p = self._procs.get(p_name)
            if not p:
                raise BackendError(f"No procedure named {p_name}")
            if getattr(p, "handler", None):
                handler = p.handler
                lim = None
                if m.group(2) and m.group(2).strip().isdigit():
                    lim = int(m.group(2).strip())
                return handler(text, t0, limit=lim)
            return self.execute(p.body)

        # DDL shortcuts
        s_text = text.strip()
        m_tbl = re.match(r"^\s*create\s+table(?:\s+if\s+not\s+exists)?\s+([A-Za-z0-9_]+)\s*\((.*)\)\s*;?\s*$", s_text, re.I | re.S)
        if m_tbl:
            tbl_name = m_tbl.group(1)
            raw_cols = m_tbl.group(2).strip()
            cols = []
            for col_def in raw_cols.split(","):
                parts = col_def.strip().split()
                if not parts:
                    continue
                cname = parts[0]
                ctype = parts[1] if len(parts) > 1 else "TEXT"
                rest = " ".join(parts[2:]).upper()
                is_pk = "PRIMARY KEY" in rest or "PK" in rest or cname.lower() == "id"
                not_null = "NOT NULL" in rest or is_pk
                cols.append((cname, ctype, not_null, is_pk))
            sid = self.create_table(tbl_name, cols)
            elapsed = (time.perf_counter() - t0) * 1000
            return Result(["result"], [(f"Table {tbl_name} created (id={sid})",)], 1, elapsed, message=f"Table {tbl_name} created")

        m_pool = re.match(r"^\s*create\s+pool(?:\s+if\s+not\s+exists)?\s+([A-Za-z0-9_\"]+)\s*;?\s*$", s_text, re.I)
        if m_pool:
            pool_name = m_pool.group(1).strip('"\'')
            sid = self.create_pool(pool_name)
            elapsed = (time.perf_counter() - t0) * 1000
            return Result(["result"], [(f"Pool {pool_name} created (id={sid})",)], 1, elapsed, message=f"Pool {pool_name} created")

        m_coll = re.match(r"^\s*create\s+collection(?:\s+if\s+not\s+exists)?\s+([A-Za-z0-9_\"]+)(?:\s+dims?\s+(\d+))?\s*;?\s*$", s_text, re.I)
        if m_coll:
            coll_name = m_coll.group(1).strip('"\'')
            dims = int(m_coll.group(2)) if m_coll.group(2) else 384
            sid = self.create_collection(coll_name, dims)
            elapsed = (time.perf_counter() - t0) * 1000
            return Result(["result"], [(f"Collection {coll_name} created (id={sid})",)], 1, elapsed, message=f"Collection {coll_name} created")

        m_link = re.match(r"^\s*create\s+link(?:\s+type)?(?:\s+if\s+not\s+exists)?\s+([A-Za-z0-9_\"]+)(?:\s+cardinality\s+([1n]:[1nm]))?\s*;?\s*$", s_text, re.I)
        if m_link:
            link_name = m_link.group(1).strip('"\'')
            card = m_link.group(2) or "n:m"
            tid = self.create_link_type(link_name, cardinality=card)
            elapsed = (time.perf_counter() - t0) * 1000
            return Result(["result"], [(f"Link type {link_name} created (id={tid})",)], 1, elapsed, message=f"Link type {link_name} created")

        m_ts = re.match(r"^\s*create\s+(?:timeseries|store)\s+(?:\"?([A-Za-z0-9_]+)\"?)(?:\s+model\s+timeseries)?\s*;?\s*$", s_text, re.I)
        if m_ts and ("timeseries" in s_text.lower() or "model" in s_text.lower()):
            ts_name = m_ts.group(1)
            sid = self.create_ts_store(ts_name)
            elapsed = (time.perf_counter() - t0) * 1000
            return Result(["result"], [(f"Time series {ts_name} created (id={sid})",)], 1, elapsed, message=f"Time series {ts_name} created")

        # Query normalization: rewrite FROM <ident> to FROM Scan("<ident>") if not already Scan(...)
        normalized = s_text
        m_del = re.match(r"^\s*delete\s+from\s+(?:table\s+|pool\s+|vectors\s+)?([A-Za-z0-9_]+)(.*)$", s_text, re.I | re.S)
        if m_del:
            tbl_target = m_del.group(1)
            rest_clause = m_del.group(2)
            try:
                sid = self._find_store_id(tbl_target)
                info = self.b_types["cdb_store_info"]()
                cat_ptr = self.b_types["get_catalog_ptr"](ct.byref(self._db))
                if c.cdb_catalog_get_store(cat_ptr, sid, ct.byref(info)) == self.b_types["CDB_OK"]:
                    if info.model == self.b_types["CDB_MODEL_RELATIONAL"]:
                        normalized = f"DELETE FROM TABLE {tbl_target}{rest_clause}"
                    elif info.model == self.b_types["CDB_MODEL_VECTOR"]:
                        normalized = f"DELETE FROM VECTORS {tbl_target}{rest_clause}"
            except Exception:
                pass
        elif not re.search(r"\bscan\s*\(", normalized, re.I):
            if re.match(r"^\s*from\s+([A-Za-z0-9_]+)", normalized, re.I):
                normalized = re.sub(r"^\s*from\s+([A-Za-z0-9_]+)", r'SELECT * FROM Scan("\1")', normalized, flags=re.I)
            else:
                normalized = re.sub(r"\bfrom\s+([A-Za-z0-9_]+)\b", r'FROM Scan("\1")', normalized, flags=re.I)

        q_bytes = normalized.strip().encode("utf-8")
        wire = ct.create_string_buffer(8192)
        wire_len = ct.c_size_t()
        err = self.b_types["cdb_compile_error"]()

        cat_ptr = self.b_types["get_catalog_ptr"](ct.byref(self._db))
        s_comp = c.cdb_compile_query(q_bytes, len(q_bytes), cat_ptr,
                                     ct.cast(wire, ct.POINTER(ct.c_uint8)), 8192,
                                     ct.byref(wire_len), ct.byref(err))
        if s_comp == self.b_types["CDB_OK"]:
            return self._run_query(wire, wire_len.value, t0)

        # A query that does not compile is a query error: compiling it as a
        # procedure would accept some of it and return a meaningless value.
        if re.match(r"\s*(?:select\b|from\b|scan\s*\()", normalized, re.I):
            err_msg = ct.string_at(err.msg).decode("utf-8") if err.msg else "Unknown CMBQL syntax error"
            raise BackendError(f"CMBQL error at line {err.line}:{err.col}: {err_msg}")

        # Fallback to procedure/mutation compilation
        bc = ct.create_string_buffer(8192)
        bc_len = ct.c_size_t()
        s_proc_comp = c.cdb_compile_proc(q_bytes, len(q_bytes), cat_ptr,
                                         ct.cast(bc, ct.POINTER(ct.c_uint8)), 8192,
                                         ct.byref(bc_len), ct.byref(err))
        if s_proc_comp == self.b_types["CDB_OK"]:
            vm = self.b_types["cdb_proc_vm"]()
            c.cdb_proc_vm_init(ct.byref(vm), ct.byref(self._db), self.b_types["CDB_ORDER_LITTLE"], None, 0, 1000000)
            ret_val = self.b_types["cdb_value"]()
            s_exec = c.cdb_proc_exec(ct.byref(vm), ct.cast(bc, ct.POINTER(ct.c_uint8)), bc_len.value, ct.byref(ret_val))
            if s_exec != self.b_types["CDB_OK"]:
                raise BackendError(f"Execution failed: {self.b_types['status_str'](self.clib, s_exec)}")
            elapsed = (time.perf_counter() - t0) * 1000
            ret_obj = self.b_types["decode_value"](ret_val, self.clib)
            return Result(["result"], [(ret_obj,)], 1, elapsed, message="Statement executed successfully")

        # Query parsing error
        err_msg = ct.string_at(err.msg).decode("utf-8") if err.msg else "Unknown CMBQL syntax error"
        raise BackendError(f"CMBQL error at line {err.line}:{err.col}: {err_msg}")

    # Query memory: operators, a scan's record buffer, and room for Sort and
    # Aggregate to buffer up to the engine's 256 rows of about 2 KB each.
    QUERY_SCRATCH = 4 * 1024 * 1024
    QUERY_MAX_ROWS = 1000

    def _field_name(self, store_id: int, field_id: int):
        """A field's name from the store's field dictionary, or None; cached."""
        names = self._field_name_cache.setdefault(store_id, {})
        if field_id in names:
            return names[field_id]
        c = self.clib.lib
        ct = self.ctypes
        nb = ct.create_string_buffer(256)
        nl = ct.c_size_t()
        cat_ptr = self.b_types["get_catalog_ptr"](ct.byref(self._db))
        if c.cdb_catalog_field_name(cat_ptr, store_id, field_id, nb, 256, ct.byref(nl)) != self.b_types["CDB_OK"]:
            return None  # not cached: the name may be added later
        names[field_id] = nb.raw[:nl.value].decode("utf-8", errors="replace")
        return names[field_id]

    def _named(self, store_id: int, obj):
        """An object decoded with field IDs as keys, keyed by field name where
        the field dictionary has one (the ID otherwise)."""
        if not isinstance(obj, dict):
            return obj
        out = {}
        for k, v in obj.items():
            name = self._field_name(store_id, k) if isinstance(k, int) else None
            out[name if name else k] = v
        return out

    def entry_label(self, store_id: int, entry: int) -> str:
        """What an entry is, in words a person (or an agent) recognizes: a
        document's summary, a table row's key and title, or for a vector the
        label of the entry it was computed from. Cached until reopen."""
        key = (store_id, entry)
        if key in self._label_cache:
            return self._label_cache[key]
        c = self.clib.lib
        ct = self.ctypes
        ok = self.b_types["CDB_OK"]
        name = self._store_names_map().get(store_id, f"store{store_id}")
        label = f"{name} {entry}"
        info = self.b_types["cdb_store_info"]()
        cat_ptr = self.b_types["get_catalog_ptr"](ct.byref(self._db))
        model = info.model if c.cdb_catalog_get_store(cat_ptr, store_id, ct.byref(info)) == ok else None
        if self._label_body is None:
            self._label_body = ct.create_string_buffer(self.DOC_MAX)
        body, blen = self._label_body, ct.c_size_t()
        if model == self.b_types["CDB_MODEL_DOCUMENT"]:
            sbuf, slen = ct.create_string_buffer(512), ct.c_size_t()
            if c.cdb_doc_get(ct.byref(self._db), store_id, entry, 0, body, self.DOC_MAX, ct.byref(blen),
                             sbuf, 512, ct.byref(slen), None) == ok and slen.value:
                label = sbuf.raw[:slen.value].decode("utf-8", errors="replace")
        elif model == self.b_types["CDB_MODEL_RELATIONAL"]:
            if c.cdb_table_get(ct.byref(self._db), store_id, entry, body, self.DOC_MAX, ct.byref(blen)) == ok:
                val = self.b_types["cdb_value"]()
                if c.cdb_value_read(ct.cast(body, ct.POINTER(ct.c_uint8)), blen.value,
                                    self.b_types["CDB_ORDER_LITTLE"], ct.byref(val)) == ok:
                    row = self._named(store_id, self.b_types["decode_value"](val, self.clib))
                    if isinstance(row, dict):
                        try:
                            pk = self.table_schema(name).rows[0][0]
                        except Exception:
                            pk = None
                        parts = [str(row[pk])] if pk in row else []
                        for col in ("title", "name"):
                            if row.get(col):
                                parts.append(str(row[col]))
                                break
                        if parts:
                            label = f"{name} " + " · ".join(parts)
        elif model == self.b_types["CDB_MODEL_VECTOR"]:
            src_store, src_entry = ct.c_uint64(), ct.c_uint64()
            if c.cdb_vec_get(ct.byref(self._db), store_id, entry, body, self.DOC_MAX, ct.byref(blen),
                             ct.byref(src_store), ct.byref(src_entry)) == ok and src_store.value:
                label = self.entry_label(src_store.value, src_entry.value)
        self._label_cache[key] = label
        return label

    def _vec_census(self, sid: int, next_id: int) -> tuple:
        """(live vectors, has a neighbour graph) for a collection. Entry IDs
        are claimed in blocks of 1024, so next_entry_id overstates the count:
        the live ones are counted, once per next_entry_id (vector_put and
        vector_delete drop the cached answer)."""
        cached = self._vec_count_cache.get(sid)
        if cached and cached[0] == next_id:
            return cached[1], cached[2]
        c = self.clib.lib
        ct = self.ctypes
        ok = self.b_types["CDB_OK"]
        buf, blen = ct.create_string_buffer(8192), ct.c_size_t()
        ss, se = ct.c_uint64(), ct.c_uint64()
        live, first = 0, 0
        for eid in range(1, next_id):
            if c.cdb_vec_get(ct.byref(self._db), sid, eid, buf, 8192, ct.byref(blen), ct.byref(ss), ct.byref(se)) == ok:
                live += 1
                first = first or eid
        has_graph = False
        if first:
            hits, n, fresh = (self.b_types["cdb_vec_hit"] * 1)(), ct.c_size_t(), ct.c_int()
            has_graph = c.cdb_vec_neighbours(ct.byref(self._db), sid, first, hits, 1, ct.byref(n), ct.byref(fresh)) == ok
        self._vec_count_cache[sid] = (next_id, live, has_graph)
        return live, has_graph

    def vector_source_label(self, coll: str, vec_id: str) -> str:
        """The entry a vector was computed from, as entry_label gives it."""
        return self.entry_label(self._find_store_id(coll), self._parse_vec_id(vec_id))

    def _run_query(self, wire, wire_len: int, t0: float) -> Result:
        c = self.clib.lib
        ct = self.ctypes
        ok = self.b_types["CDB_OK"]
        status_str = self.b_types["status_str"]

        cur = self.b_types["cdb_exec_cursor"]()
        scratch = ct.create_string_buffer(self.QUERY_SCRATCH)
        s_open = c.cdb_exec_open(ct.byref(cur), ct.byref(self._db),
                                 ct.cast(wire, ct.POINTER(ct.c_uint8)), wire_len,
                                 None, 0,
                                 ct.cast(scratch, ct.POINTER(ct.c_uint8)), self.QUERY_SCRATCH)
        if s_open != ok:
            raise BackendError(f"Query could not start: {status_str(self.clib, s_open)}")

        cols: list = []
        records: list = []
        stores: set = set()
        truncated = False
        out_row = self.b_types["cdb_exec_row"]()
        has_row = ct.c_int(0)
        try:
            while True:
                s_nxt = c.cdb_exec_next(ct.byref(cur), ct.byref(out_row), ct.byref(has_row))
                if s_nxt == -19:  # CDB_E_OUT_OF_BUDGET
                    raise BackendError("Query too large: Sort and GROUP BY hold at most 256 rows in the engine "
                                       "today. Narrow it with WHERE.")
                if s_nxt != ok:
                    raise BackendError(f"Query failed after {len(records)} rows: {status_str(self.clib, s_nxt)}")
                if not has_row.value:
                    break
                if len(records) >= self.QUERY_MAX_ROWS:
                    truncated = True
                    break
                if out_row.n_fields > 0:
                    # Output fields: a projection, aggregate or join
                    rec = {}
                    for f_idx in range(out_row.n_fields):
                        fld = out_row.fields[f_idx]
                        fname = ct.string_at(fld.name, fld.name_len).decode("utf-8", errors="replace")
                        rec[fname] = self.b_types["decode_value"](fld.val, self.clib)
                elif out_row.record_data and out_row.record_len:
                    # SELECT *: the whole record, by field name
                    rec = {"$id": out_row.entry_id}
                    val = self.b_types["cdb_value"]()
                    if c.cdb_value_read(out_row.record_data, out_row.record_len,
                                        self.b_types["CDB_ORDER_LITTLE"], ct.byref(val)) == ok:
                        named = self._named(out_row.store_id, self.b_types["decode_value"](val, self.clib))
                        rec.update(named if isinstance(named, dict) else {"value": named})
                else:
                    rec = {"$id": out_row.entry_id}
                for k in rec:
                    if k not in cols:
                        cols.append(k)
                records.append(rec)
                stores.add(out_row.store_id)
        finally:
            c.cdb_exec_close(ct.byref(cur))

        # Whole table rows: the table's column order. A row stores no NULL
        # columns, so the order they were first seen in is arbitrary.
        if len(stores) == 1 and cols and cols[0] == "$id":
            name = self._store_names_map().get(next(iter(stores)))
            try:
                order = [r[0] for r in self.table_schema(name).rows] if name else []
            except Exception:
                order = []  # not a table
            if order:
                cols = ["$id"] + order + [k for k in cols if k != "$id" and k not in order]

        rows = [tuple(r.get(k) for k in cols) for r in records]
        elapsed = (time.perf_counter() - t0) * 1000
        msg = f"Executed CMBQL query ({len(rows)} rows)"
        if truncated:
            msg = f"Executed CMBQL query (first {len(rows)} rows shown; there are more)"
        return Result([str(k) for k in cols] or ["$id"], rows, len(rows), elapsed, message=msg)

    def explain(self, text: str) -> Result:
        t0 = time.perf_counter()
        s_text = text.strip()

        # Check DDL shortcuts
        if re.match(r"^\s*create\s+", s_text, re.I):
            words = s_text.split()
            target = words[1].upper() if len(words) > 1 else "OBJECT"
            return Result(["step", "operator", "target", "details"],
                          [(1, "DDL", target, f"Execute DDL statement: {s_text}")],
                          1, (time.perf_counter() - t0) * 1000, message="DDL execution plan")

        c = self.clib.lib
        ct = self.ctypes

        # Normalize query
        normalized = s_text
        if not re.search(r"\bscan\s*\(", normalized, re.I):
            if re.match(r"^\s*from\s+([A-Za-z0-9_]+)", normalized, re.I):
                normalized = re.sub(r"^\s*from\s+([A-Za-z0-9_]+)", r'SELECT * FROM Scan("\1")', normalized, flags=re.I)
            else:
                normalized = re.sub(r"\bfrom\s+([A-Za-z0-9_]+)\b", r'FROM Scan("\1")', normalized, flags=re.I)

        q_bytes = normalized.strip().encode("utf-8")
        wire = ct.create_string_buffer(8192)
        wire_len = ct.c_size_t()
        err = self.b_types["cdb_compile_error"]()
        cat_ptr = self.b_types["get_catalog_ptr"](ct.byref(self._db))
        s_comp = c.cdb_compile_query(q_bytes, len(q_bytes), cat_ptr,
                                     ct.cast(wire, ct.POINTER(ct.c_uint8)), 8192,
                                     ct.byref(wire_len), ct.byref(err))

        if s_comp != self.b_types["CDB_OK"]:
            # Check proc compilation
            bc = ct.create_string_buffer(8192)
            bc_len = ct.c_size_t()
            s_proc = c.cdb_compile_proc(q_bytes, len(q_bytes), cat_ptr,
                                        ct.cast(bc, ct.POINTER(ct.c_uint8)), 8192,
                                        ct.byref(bc_len), ct.byref(err))
            if s_proc == self.b_types["CDB_OK"]:
                elapsed = (time.perf_counter() - t0) * 1000
                return Result(["step", "operator", "target", "details"],
                              [(1, "PROC_VM", "bytecode", f"Execute compiled mutation bytecode ({bc_len.value} bytes)")],
                              1, elapsed, message="Procedure execution plan")
            err_msg = ct.string_at(err.msg).decode("utf-8") if err.msg else "Unknown CMBQL syntax error"
            raise BackendError(f"CMBQL error at line {err.line}:{err.col}: {err_msg}")

        TAG_OPS = {
            0x01: "SCAN",
            0x02: "FILTER",
            0x03: "PROJECT",
            0x04: "JOIN",
            0x05: "SORT",
            0x06: "LIMIT",
            0x07: "UNION",
            0x08: "AGGREGATE",
            0x09: "TRAVERSE",
            0x0A: "TEXT_SEARCH",
            0x0B: "VECTOR_SEARCH",
            0x0C: "HYBRID_RANK",
            0x0D: "WINDOW",
            0x0E: "ASOF_JOIN",
            0x0F: "UNNEST",
        }

        data = bytes(wire)[:wire_len.value]
        idx = 6  # skip 6-byte query wire header
        plan_steps = []
        step_num = 1

        while idx < len(data):
            tag = data[idx]
            op_name = TAG_OPS.get(tag, f"OP_{tag:#04x}")
            idx += 1
            if tag == 0x01:  # SCAN
                s_len = 0
                shift = 0
                while idx < len(data):
                    b = data[idx]
                    idx += 1
                    s_len |= (b & 0x7F) << shift
                    if not (b & 0x80):
                        break
                    shift += 7
                s_name = data[idx:idx + s_len].decode("utf-8", errors="replace")
                idx += s_len
                plan_steps.append((step_num, op_name, s_name, f"Sequential scan on store '{s_name}'"))
            elif tag == 0x06:  # LIMIT
                count = 0
                shift = 0
                while idx < len(data):
                    b = data[idx]
                    idx += 1
                    count |= (b & 0x7F) << shift
                    if not (b & 0x80):
                        break
                    shift += 7
                offset = 0
                shift = 0
                while idx < len(data):
                    b = data[idx]
                    idx += 1
                    offset |= (b & 0x7F) << shift
                    if not (b & 0x80):
                        break
                    shift += 7
                plan_steps.append((step_num, op_name, f"limit={count}", f"Limit stream to {count} rows (offset={offset})"))
            elif tag == 0x02:  # FILTER
                plan_steps.append((step_num, op_name, "predicate", "Filter stream rows based on condition"))
            elif tag == 0x03:  # PROJECT
                passthrough = data[idx] if idx < len(data) else 0
                idx += 1
                plan_steps.append((step_num, op_name, "columns", f"Project output columns (passthrough={passthrough})"))
                break
            elif tag == 0x05:  # SORT
                plan_steps.append((step_num, op_name, "order", "Sort stream by key"))
                break
            else:
                plan_steps.append((step_num, op_name, "", f"Operator {op_name}"))
                break
            step_num += 1

        plan_steps.reverse()
        for i, item in enumerate(plan_steps):
            plan_steps[i] = (i + 1, item[1], item[2], item[3])

        elapsed = (time.perf_counter() - t0) * 1000
        return Result(["step", "operator", "target", "details"], plan_steps, len(plan_steps), elapsed, message=f"Execution plan ({len(plan_steps)} steps)")



# ---------------------------------------------------------------- mock
_WORDS = ("river glass winter orchard lantern harbor cipher meadow ember north quiet "
          "iron silver garden letters voyage hollow tide").split()
_FIRST = "Ada Mary Jane Henry Edith Oscar Leo Virginia Walt Emily Arthur Louisa".split()
_LAST = "Shelley Austen James Wharton Wilde Tolstoy Woolf Whitman Bronte Doyle Alcott Hardy".split()


def _title(r: random.Random) -> str:
    words = " ".join(r.choice(_WORDS).capitalize() for _ in range(r.randint(2, 4)))
    return ("The " if r.random() < 0.4 else "") + words


def _cos_dist(a, b):
    dot = sum(x * y for x, y in zip(a, b))
    na = math.sqrt(sum(x * x for x in a)) or 1
    nb = math.sqrt(sum(y * y for y in b)) or 1
    return 1 - dot / (na * nb)


class MockBackend(Backend):
    """Deterministic, mutable fake data shaped like the GutenFree demo."""

    name = "mock"
    VEC_DIMS = 24
    _TABLES = {
        "books": (["id", "title", "author_id", "year", "lang", "downloads"],
                  ["INT", "TEXT", "INT", "INT", "TEXT", "INT"], 12480),
        "authors": (["id", "name", "born", "died"], ["INT", "TEXT", "INT", "INT"], 3120),
        "loans": (["id", "book_id", "reader", "started_at", "returned"],
                  ["INT", "INT", "TEXT", "TIMESTAMP", "BOOL"], 48213),
    }
    _DOC_POOLS = {"catalog": 9876, "reviews": 2210}
    _LINK_META = {
        "written_by": {"from": "books", "to": "authors", "cardinality": "n:1", "on_delete": "restrict"},
        "similar_to": {"from": "catalog", "to": "catalog", "cardinality": "n:m", "on_delete": "cascade"},
    }

    def __init__(self):
        self.path = None
        self._tables, self._revs, self._docs = {}, {}, {}
        self._doc_created, self._doc_deleted = {}, {}
        self._vec, self._vgraph = {}, {}
        self._links, self._ts_extra = {}, {}
        self._procs = {p.name: p for p in (
            Procedure("recent_books", ["$year"], "from books\nwhere year >= $year\norder by year desc\nlimit 50", "Books since a year"),
            Procedure("popular_by_lang", ["$lang", "$min"],
                      "from books\nwhere lang = $lang and downloads > $min\norder by downloads desc\nlimit 25", "Top downloads in a language"),
        )}

    def open(self, path): self.path = path
    def create(self, path): self.path = path

    def file_info(self):
        return {"Path": self.path, "Size": "18.4 MiB", "Page size": "4096 B", "Format": "v0 (unstable)",
                "Journal": "rollback · fsync on commit", "Last checkpoint": time.strftime("%Y-%m-%d %H:%M")}

    def list_stores(self):
        s = [Store(n, Model.TABLE, len(self._tables[n]) if n in self._tables else c)
             for n, (_, _, c) in self._TABLES.items()]
        for pool, n in self._DOC_POOLS.items():
            n += len(self._doc_created.get(pool, [])) - len(self._doc_deleted.get(pool, set()))
            s.append(Store(pool, Model.DOCUMENT, n, meta={"fts": "bm25"}))
        s.append(Store("book_embeddings", Model.VECTOR, len(self._vectors("book_embeddings")),
                       meta={"dims": self.VEC_DIMS, "metric": "cosine", "index": "neighbour graph"}))
        s.append(Store("title_vecs", Model.VECTOR, len(self._vectors("title_vecs")),
                       meta={"dims": self.VEC_DIMS, "metric": "cosine", "index": "exact"}))
        for name, meta in self._LINK_META.items():
            s.append(Store(name, Model.LINK, len(self._link_rows(name)), meta=meta))
        s.append(Store("reads_per_min", Model.TIMESERIES, 525600, meta={"unit": "reads/min", "late_arrival": "merge"}))
        s.append(Store("cache_hits", Model.TIMESERIES, 131400, meta={"unit": "%", "late_arrival": "reject"}))
        return s

    # ---- tables
    @staticmethod
    def _gen_row(table, r, rid):
        if table == "books":
            return (rid, _title(r), r.randint(1, 3120), r.randint(1600, 1925),
                    r.choice(["en", "en", "en", "fr", "de", "es"]), r.randint(0, 90000))
        if table == "authors":
            born = r.randint(1550, 1900)
            return (rid, f"{r.choice(_FIRST)} {r.choice(_LAST)}", born,
                    born + r.randint(25, 90) if r.random() > 0.05 else None)
        return (rid, r.randint(1, 12480), f"reader-{r.randint(1, 900):04d}",
                f"2026-{r.randint(1, 10):02d}-{r.randint(1, 28):02d} {r.randint(0, 23):02d}:{r.randint(0, 59):02d}",
                r.random() > 0.3)

    def _table(self, name):
        if name not in self._TABLES:
            raise BackendError(f"No table {name}")
        if name not in self._tables:
            r = random.Random(name)
            self._tables[name] = [self._gen_row(name, r, i + 1) for i in range(self._TABLES[name][2])]
        return self._tables[name]

    def table_schema(self, name):
        cols, types, _ = self._TABLES[name]
        rows = [(c, t, "NO" if c == "id" else "YES", None,
                 "PRIMARY KEY" if c == "id" else ("FOREIGN KEY" if c.endswith("_id") else ""))
                for c, t in zip(cols, types)]
        return Result(["column", "type", "nullable", "default", "constraints"], rows)

    def table_indexes(self, name):
        cols, _, _ = self._TABLES[name]
        rows = [(f"{name}_pkey", "id", "btree", True)]
        rows += [(f"{name}_{c}_idx", c, "btree", False) for c in cols if c.endswith("_id")]
        return Result(["index", "columns", "kind", "unique"], rows)

    def table_rows(self, name, offset, limit, filter_text=""):
        t = time.perf_counter()
        data = self._table(name)
        if filter_text:
            q = filter_text.lower()
            data = [row for row in data if any(q in str(v).lower() for v in row)]
        return Result(self._TABLES[name][0], data[offset:offset + limit], len(data), (time.perf_counter() - t) * 1000)

    def table_insert(self, name, values):
        cols = self._TABLES[name][0]
        data = self._table(name)
        pk = (max((r[0] for r in data), default=0)) + 1
        data.append(tuple(pk if c == "id" else values.get(c) for c in cols))
        return pk

    def table_update(self, name, pk, column, value):
        cols = self._TABLES[name][0]
        if column == "id":
            raise BackendError("Primary keys are immutable")
        i = cols.index(column)
        data = self._table(name)
        for n, row in enumerate(data):
            if row[0] == pk:
                data[n] = row[:i] + (value,) + row[i + 1:]
                return
        raise BackendError(f"Row {pk} no longer exists")

    def table_delete(self, name, pks):
        drop = set(pks)
        data = self._table(name)
        before = len(data)
        data[:] = [r for r in data if r[0] not in drop]
        return before - len(data)

    # ---- documents
    def _doc(self, pool, i):
        r = random.Random(f"{pool}:{i}")
        return DocHit(f"doc-{i + 1:05d}", _title(r), r.randint(1, 6))

    def _all_hits(self, pool, n):
        deleted = self._doc_deleted.get(pool, set())
        created = [DocHit(d, self.doc_get(pool, d).get("title", "Untitled"), self.doc_revisions(pool, d)[-1])
                   for d in reversed(self._doc_created.get(pool, []))]
        base = (self._doc(pool, i) for i in range(n))
        return [h for h in (*created, *base) if h.id not in deleted]

    def doc_list(self, pool, limit=200):
        return self._all_hits(pool, limit)[:limit]

    def fts_search(self, pool, query, limit=50):
        terms = query.lower().split()
        hits = []
        for d in self._all_hits(pool, 2000):
            s = sum(d.title.lower().count(w) for w in terms)
            if s:
                d.score = round(s * 2.1 + random.Random(d.id).random() * 1.5, 3)
                hits.append(d)
        return sorted(hits, key=lambda d: -d.score)[:limit]

    def doc_revisions(self, pool, doc_id):
        if doc_id in self._doc_deleted.get(pool, set()):
            raise BackendError(f"{doc_id} was deleted")
        base = self._revs.get((pool, doc_id)) or self._doc(pool, int(doc_id.split("-")[1]) - 1).rev
        return list(range(1, base + 1))

    def doc_get(self, pool, doc_id, rev=None):
        rev = rev or self.doc_revisions(pool, doc_id)[-1]
        if (pool, doc_id, rev) in self._docs:
            return self._docs[(pool, doc_id, rev)]
        i = int(doc_id.split("-")[1]) - 1
        r = random.Random(f"{pool}:{i}:{rev}")
        return {"_id": doc_id, "_rev": rev, "title": self._doc(pool, i).title,
                "author": f"{r.choice(_FIRST)} {r.choice(_LAST)}", "language": r.choice(["en", "fr", "de"]),
                "subjects": r.sample(_WORDS, 3), "downloads": r.randint(0, 90000),
                "excerpt": " ".join(r.choice(_WORDS) for _ in range(18)) + "."}

    def doc_put(self, pool, doc_id, doc):
        if not isinstance(doc, dict):
            raise BackendError("A document must be a JSON object")
        n = self.doc_revisions(pool, doc_id)[-1] + 1
        self._revs[(pool, doc_id)] = n
        self._docs[(pool, doc_id, n)] = {**doc, "_id": doc_id, "_rev": n}
        return n

    def doc_create(self, pool, doc):
        n = self._DOC_POOLS[pool] + len(self._doc_created.get(pool, [])) + 1
        doc_id = f"doc-{n:05d}"
        self._revs[(pool, doc_id)] = 1
        self._docs[(pool, doc_id, 1)] = {**doc, "_id": doc_id, "_rev": 1}
        self._doc_created.setdefault(pool, []).append(doc_id)
        return doc_id

    def doc_delete(self, pool, doc_id):
        self._doc_deleted.setdefault(pool, set()).add(doc_id)

    # ---- vectors
    def _vectors(self, coll):
        if coll not in self._vec:
            r = random.Random(coll)
            centers = [[r.gauss(0, 1) for _ in range(self.VEC_DIMS)] for _ in range(6)]
            self._vec[coll] = {f"vec-{i + 1:05d}": [x + r.gauss(0, 0.45) for x in centers[i % 6]] for i in range(220)}
        return self._vec[coll]

    def _parse_query(self, coll, query):
        data = self._vectors(coll)
        q = query.strip()
        if not q:
            raise BackendError("Enter an entry id or a vector literal.")
        if q.startswith("["):
            try:
                vec = [float(x) for x in json.loads(q)]
            except (ValueError, TypeError):
                raise BackendError("Vector literal must be a JSON array of numbers")
            return None, (vec + [0.0] * self.VEC_DIMS)[:self.VEC_DIMS]
        if q not in data:
            raise BackendError(f"No entry {q} in {coll}")
        return q, data[q]

    def vector_search(self, coll, query, k, approximate=False):
        qid, vec = self._parse_query(coll, query)
        scored = sorted((_cos_dist(vec, v), vid) for vid, v in self._vectors(coll).items() if vid != qid)
        return [(vid, round(d, 4)) for d, vid in scored[:k]]

    def vector_graph(self, coll, limit=220):
        if coll not in self._vgraph:
            data = self._vectors(coll)
            ids = list(data)[:limit]
            vecs = [data[i] for i in ids]
            edges = set()
            for i, a in enumerate(vecs):
                near = sorted(range(len(vecs)), key=lambda j: _cos_dist(a, vecs[j]) if j != i else 9)[:3]
                edges.update((min(i, j), max(i, j)) for j in near)
            self._vgraph[coll] = VectorGraph(ids, vecs, sorted(edges))
        return self._vgraph[coll]

    def vector_get(self, coll, vec_id):
        return self._parse_query(coll, vec_id)[1]

    def vector_put(self, coll, vec_id, vec):
        if len(vec) != self.VEC_DIMS:
            raise BackendError(f"{coll} expects {self.VEC_DIMS} dimensions, got {len(vec)}")
        self._vectors(coll)[vec_id] = list(vec)
        self._vgraph.pop(coll, None)

    def vector_delete(self, coll, vec_id):
        if self._vectors(coll).pop(vec_id, None) is None:
            raise BackendError(f"No entry {vec_id}")
        self._vgraph.pop(coll, None)

    # ---- links
    def _link_rows(self, name):
        if name not in self._links:
            r = random.Random(name)
            date = lambda: f"2026-{r.randint(1, 10):02d}-{r.randint(1, 28):02d}"
            rows = []
            if name == "written_by":
                rows = [(f"book-{b:05d}", f"author-{r.randint(1, 45):04d}", date(), round(r.uniform(0.2, 1.0), 3)) for b in range(1, 181)]
            else:
                seen = set()
                for _ in range(260):
                    c = r.randint(0, 7)
                    a, b = c * 18 + r.randint(1, 18), c * 18 + r.randint(1, 18)
                    cross = r.random() < 0.08
                    if cross:
                        b = r.randint(1, 144)
                    if a != b and (a, b) not in seen:
                        seen.add((a, b))
                        rows.append((f"doc-{a:05d}", f"doc-{b:05d}", date(), round(r.uniform(0.05, 0.35) if cross else r.uniform(0.55, 1.0), 3)))
            self._links[name] = rows
        return self._links[name]

    def link_edges(self, name, limit=500):
        t = time.perf_counter()
        rows = self._link_rows(name)
        return Result(["source", "target", "created", "weight"], rows[:limit], len(rows), (time.perf_counter() - t) * 1000)

    def link_add(self, name, source, target, weight=1.0):
        source, target = source.strip(), target.strip()
        if not source or not target:
            raise BackendError("Source and target are required")
        rows = self._link_rows(name)
        if self._LINK_META[name]["cardinality"] == "n:1":
            for s, t, *_ in rows:
                if s == source:
                    raise BackendError(f"{name} is n:1 — {source} already links to {t}")
        if any(s == source and t == target for s, t, *_ in rows):
            raise BackendError("That link already exists")
        rows.insert(0, (source, target, time.strftime("%Y-%m-%d"), round(float(weight), 3)))

    def link_delete(self, name, pairs):
        drop = set(pairs)
        rows = self._link_rows(name)
        before = len(rows)
        rows[:] = [r for r in rows if (r[0], r[1]) not in drop]
        return before - len(rows)

    # ---- time series
    def ts_series_list(self, name):
        return [name]

    def ts_points(self, name, window_s, series=None):
        now, n = time.time(), 180
        r = random.Random(f"{name}:{window_s}")
        pts = []
        for i in range(n):
            t = now - window_s + window_s * i / (n - 1)
            base = 88 + 6 * math.sin(i / 20) if name == "cache_hits" else 60 + 25 * math.sin(i / 14) + 10 * math.sin(i / 5.3)
            pts.append((t, max(0.0, base + r.gauss(0, 4))))
        pts += [p for p in self._ts_extra.get(name, []) if now - window_s <= p[0] <= now + 3600]
        return sorted(pts)

    def ts_append(self, name, t, value, series=None):
        if name == "cache_hits" and t < time.time() - 3600:
            raise BackendError("cache_hits rejects late arrivals older than 1 h")
        self._ts_extra.setdefault(name, []).append((t, value))

    # ---- query
    def proc_list(self):
        return sorted(self._procs.values(), key=lambda p: p.name)

    def proc_save(self, proc):
        self._procs[proc.name] = proc

    def proc_delete(self, name):
        if self._procs.pop(name, None) is None:
            raise BackendError(f"No procedure {name}")

    def execute(self, text):
        m = re.match(r"\s*call\s+(\w+)", text, re.I)
        if m:
            p = self._procs.get(m.group(1))
            if not p:
                raise BackendError(f"No procedure named {m.group(1)}")
            res = self.execute(p.body)
            res.message = f"mock · call {p.name}"
            return res
        q = text.lower()
        for name in self._TABLES:
            if name in q:
                m = re.search(r"limit\s+(\d+)", q)
                res = self.table_rows(name, 0, min(int(m.group(1)) if m else 50, 1000))
                res.message = f"mock · scanned {name}"
                return res
        raise BackendError("CMBQL isn't implemented in the engine yet. The mock answers any "
                           "query that names a table, e.g.  from books limit 20")

    def explain(self, text: str) -> Result:
        t0 = time.perf_counter()
        q = text.lower()
        rows = []
        for name in self._TABLES:
            if name in q:
                rows.append((1, "SCAN", name, f"Full table scan on '{name}'"))
                m = re.search(r"limit\s+(\d+)", q)
                if m:
                    rows.append((2, "LIMIT", f"limit={m.group(1)}", f"Take up to {m.group(1)} rows"))
                break
        if not rows:
            rows.append((1, "EVAL", "statement", "Execute statement in mock VM"))
        return Result(["step", "operator", "target", "details"], rows, len(rows), (time.perf_counter() - t0) * 1000, message="Mock execution plan")
