import os
import shutil
import time
import unittest
from pathlib import Path
from combolite_gui.backend import NativeBackend, Model, Procedure, BackendError


class TestNativeBackend(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.db_dir = "/tmp/combolite_studio_test"
        cls.db_path = f"{cls.db_dir}/test_studio.cdbarch"
        os.makedirs(cls.db_dir, exist_ok=True)

    @classmethod
    def tearDownClass(cls):
        cls._cleanup()

    @classmethod
    def _cleanup(cls):
        base = "/tmp/combolite_studio_test/test_studio"
        for p in [f"{base}.cdbarch", f"{base}.cdbwork"]:
            if os.path.exists(p):
                if os.path.isdir(p):
                    shutil.rmtree(p)
                else:
                    os.remove(p)

    def setUp(self):
        self._cleanup()
        self.backend = NativeBackend()

    def tearDown(self):
        if hasattr(self, "backend") and self.backend and self.backend.is_open:
            self.backend.close()
        self._cleanup()

    def test_01_lifecycle(self):
        self.backend.create(self.db_path)
        self.assertTrue(self.backend.is_open)
        info = self.backend.file_info()
        self.assertIn("Path", info)
        self.assertEqual(info["Path"], self.db_path)
        self.backend.close()
        self.assertFalse(self.backend.is_open)

        # Reopen
        self.backend.open(self.db_path)
        self.assertTrue(self.backend.is_open)
        self.backend.close()

    def test_02_tables(self):
        self.backend.create(self.db_path)

        # Create table via DDL execution
        res = self.backend.execute("CREATE TABLE users (id INT PRIMARY KEY, name TEXT, age INT);")
        self.assertEqual(res.columns, ["result"])

        stores = self.backend.list_stores()
        tbl_store = next((s for s in stores if s.name == "users"), None)
        self.assertIsNotNone(tbl_store)
        self.assertEqual(tbl_store.model, Model.TABLE)

        # Insert rows
        pk1 = self.backend.table_insert("users", {"id": 1, "name": "Alice", "age": 30})
        self.assertEqual(pk1, 1)
        pk2 = self.backend.table_insert("users", {"id": 2, "name": "Bob", "age": 25})
        self.assertEqual(pk2, 2)

        # Schema
        schema = self.backend.table_schema("users")
        col_names = [r[0] for r in schema.rows]
        self.assertIn("id", col_names)
        self.assertIn("name", col_names)
        self.assertIn("age", col_names)

        # Rows
        rows_res = self.backend.table_rows("users", 0, 10)
        self.assertEqual(rows_res.total, 2)
        self.assertEqual(len(rows_res.rows), 2)

        # Filter
        filtered = self.backend.table_rows("users", 0, 10, filter_text="Alice")
        self.assertEqual(filtered.total, 1)

        # Update
        self.backend.table_update("users", pk1, "age", 31)
        rows_res2 = self.backend.table_rows("users", 0, 10)
        alice_row = next(r for r in rows_res2.rows if r[0] == 1)
        age_idx = rows_res2.columns.index("age")
        self.assertEqual(alice_row[age_idx], 31)

        # Delete
        del_count = self.backend.table_delete("users", [pk2])
        self.assertEqual(del_count, 1)
        rows_res3 = self.backend.table_rows("users", 0, 10)
        self.assertEqual(rows_res3.total, 1)

    def test_03_documents(self):
        self.backend.create(self.db_path)

        # Create document pool
        self.backend.execute("CREATE POOL articles;")
        doc_id = self.backend.doc_create("articles", {"title": "First Article", "content": "Hello World"})
        self.assertTrue(doc_id.startswith("doc-"))

        # doc_get
        doc = self.backend.doc_get("articles", doc_id)
        self.assertEqual(doc["title"], "First Article")
        self.assertEqual(doc["content"], "Hello World")
        self.assertEqual(doc["_rev"], 1)

        # doc_put (update rev)
        new_rev = self.backend.doc_put("articles", doc_id, {"title": "Updated Article", "content": "Hello Universe"})
        self.assertEqual(new_rev, 2)
        doc_v2 = self.backend.doc_get("articles", doc_id, rev=2)
        self.assertEqual(doc_v2["title"], "Updated Article")
        self.assertEqual(doc_v2["content"], "Hello Universe")

        # doc_revisions
        revs = self.backend.doc_revisions("articles", doc_id)
        self.assertEqual(revs, [1, 2])

        # doc_list
        hits = self.backend.doc_list("articles", limit=10)
        self.assertTrue(len(hits) >= 1)
        self.assertEqual(hits[0].id, doc_id)

        # doc_delete
        self.backend.doc_delete("articles", doc_id)
        with self.assertRaises(BackendError):
            self.backend.doc_get("articles", doc_id)

    def test_04_vectors(self):
        self.backend.create(self.db_path)

        # Create collection
        self.backend.execute("CREATE COLLECTION embeddings DIMS 4;")
        self.backend.vector_put("embeddings", "vec-00001", [1.0, 0.0, 0.0, 0.0])
        self.backend.vector_put("embeddings", "vec-00002", [0.0, 1.0, 0.0, 0.0])

        # vector_get
        v1 = self.backend.vector_get("embeddings", "vec-00001")
        self.assertEqual(len(v1), 4)
        self.assertAlmostEqual(v1[0], 1.0)

        # vector_search
        search_res = self.backend.vector_search("embeddings", "[1.0, 0.0, 0.0, 0.0]", 2)
        self.assertEqual(len(search_res), 2)
        self.assertEqual(search_res[0][0], "vec-00001")
        self.assertAlmostEqual(search_res[0][1], 0.0, places=3)

        # vector_graph
        graph = self.backend.vector_graph("embeddings")
        self.assertIn("vec-00001", graph.ids)
        self.assertIn("vec-00002", graph.ids)
        self.assertEqual(len(graph.vectors), 2)

        # vector_delete
        self.backend.vector_delete("embeddings", "vec-00002")
        with self.assertRaises(BackendError):
            self.backend.vector_get("embeddings", "vec-00002")

    def test_05_links(self):
        self.backend.create(self.db_path)

        self.backend.execute("CREATE TABLE authors (id INT PRIMARY KEY, name TEXT);")
        self.backend.execute("CREATE TABLE books (id INT PRIMARY KEY, title TEXT);")
        a_pk = self.backend.table_insert("authors", {"id": 1, "name": "Author A"})
        b_pk = self.backend.table_insert("books", {"id": 1, "title": "Book 101"})

        self.backend.execute("CREATE LINK written_by CARDINALITY n:1;")
        self.backend.link_add("written_by", f"books-{b_pk}", f"authors-{a_pk}", weight=1.0)

        edges = self.backend.link_edges("written_by")
        self.assertEqual(edges.total, 1)
        self.assertEqual(edges.rows[0][0], f"books-{b_pk}")
        self.assertEqual(edges.rows[0][1], f"authors-{a_pk}")

        del_cnt = self.backend.link_delete("written_by", [(f"books-{b_pk}", f"authors-{a_pk}")])
        self.assertEqual(del_cnt, 1)
        edges2 = self.backend.link_edges("written_by")
        self.assertEqual(edges2.total, 0)

    def test_06_timeseries(self):
        self.backend.create(self.db_path)

        self.backend.execute("CREATE TIMESERIES metrics;")
        now = time.time()
        self.backend.ts_append("metrics", now, 42.5)

        pts = self.backend.ts_points("metrics", 3600)
        self.assertTrue(len(pts) >= 1)
        self.assertAlmostEqual(pts[-1][1], 42.5, places=1)

    def test_07_procedures_and_queries(self):
        self.backend.create(self.db_path)

        self.backend.execute("CREATE TABLE users (id INT PRIMARY KEY, name TEXT, age INT);")
        self.backend.table_insert("users", {"id": 1, "name": "Alice", "age": 30})
        self.backend.table_insert("users", {"id": 2, "name": "Bob", "age": 25})

        initial_count = len(self.backend.proc_list())
        p = Procedure("get_users", ["$a"], "FROM users LIMIT 5", "Retrieve users")
        self.backend.proc_save(p)
        procs = self.backend.proc_list()
        self.assertEqual(len(procs), initial_count + 1)
        self.assertIn("get_users", [pr.name for pr in procs])

        # Call procedure
        call_res = self.backend.execute("CALL get_users")
        self.assertEqual(call_res.total, 2)

        # Direct query with Scan
        q_res = self.backend.execute("SELECT * FROM Scan(\"users\") LIMIT 10;")
        self.assertEqual(q_res.total, 2)

        # Direct query with rewritten from
        q_res2 = self.backend.execute("FROM users LIMIT 10")
        self.assertEqual(q_res2.total, 2)

        self.backend.proc_delete("get_users")
        self.assertEqual(len(self.backend.proc_list()), initial_count)

    def test_08_explain_plan(self):
        self.backend.create(self.db_path)
        self.backend.execute("CREATE TABLE users (id INT PRIMARY KEY, name TEXT, age INT);")

        # Explain DDL
        plan_ddl = self.backend.explain("CREATE POOL my_docs;")
        self.assertEqual(plan_ddl.columns, ["step", "operator", "target", "details"])
        self.assertTrue(len(plan_ddl.rows) >= 1)

        # Explain Query
        plan_q = self.backend.explain("SELECT * FROM Scan(\"users\") LIMIT 10;")
        self.assertEqual(plan_q.columns, ["step", "operator", "target", "details"])
        self.assertTrue(len(plan_q.rows) >= 1)
        ops = [r[1] for r in plan_q.rows]
        self.assertIn("SCAN", ops)
        self.assertIn("LIMIT", ops)

    def test_09_open_large_demo_database(self):
        demo_path = Path(__file__).resolve().parents[3] / "db-combolite" / "demo" / "gutenfree" / "data" / "gutenfree.cdbarch"
        if not demo_path.exists():
            self.skipTest("gutenfree.cdbarch demo database not found")

        self.backend.open(str(demo_path))
        stores = self.backend.list_stores()
        self.assertTrue(len(stores) > 0)
        store_map = {s.name: s for s in stores}
        self.assertIn("books", store_map)
        self.assertIn("passages", store_map)
        self.assertIn("passage_vec", store_map)
        self.assertIn("similar_to", store_map)
        self.assertIn("metrics", store_map)

        # Verify accurate counts
        self.assertEqual(store_map["books"].count, 79875)
        self.assertEqual(store_map["passages"].count, 16033)
        self.assertEqual(store_map["passage_vec"].count, 16033)
        self.assertEqual(store_map["passage_bits"].count, 16033)
        self.assertEqual(store_map["book_vec"].count, 121)
        self.assertEqual(store_map["similar_to"].count, 715)
        self.assertEqual(store_map["metrics"].count, 28097)

        # Verify reading rows from the 79k+ books table
        rows = self.backend.table_rows("books", 0, 10)
        self.assertTrue(len(rows.rows) > 0)
        self.assertIn("title", rows.columns)
        self.assertIn("author", rows.columns)

        # Verify reading document
        docs = self.backend.doc_list("passages", limit=10)
        self.assertTrue(len(docs) > 0)
        doc = self.backend.doc_get("passages", docs[0].id)
        self.assertIn("_id", doc)

        # Verify reading link edges
        edges = self.backend.link_edges("similar_to")
        self.assertEqual(edges.total, 715)
        self.assertTrue(len(edges.rows) > 0)

        # Verify reading timeseries series and points
        series = self.backend.ts_series_list("metrics")
        self.assertTrue(len(series) > 0)
        pts = self.backend.ts_points("metrics", 86400)
        self.assertTrue(len(pts) > 0)

        # Verify call ingestion_records and call downloaded_books
        ing_res = self.backend.execute("call ingestion_records")
        self.assertEqual(len(ing_res.rows), 121)
        self.assertIn("book_vector", ing_res.columns)
        self.assertIn("similar_links", ing_res.columns)

        dl_res = self.backend.execute("call downloaded_books")
        self.assertEqual(ing_res.rows, dl_res.rows)

        # Verify from books does not hit the shortcut
        from_res = self.backend.execute("from books where passages > 0 limit 5")
        self.assertNotIn("book_vector", from_res.columns)
        self.assertIn("title", from_res.columns)

    def test_10_ingestion_data_missing_error(self):
        self.backend.create(self.db_path)
        with self.assertRaises(BackendError) as ctx:
            self.backend.execute("call ingestion_records")
        self.assertIn("this database has no ingestion data", str(ctx.exception))

    def test_11_console_queries_read_real_rows(self):
        self.backend.create(self.db_path)
        self.backend.execute("CREATE TABLE users (id INT PRIMARY KEY, name TEXT, years INT);")
        for pk, name, age in [(1, "Alice", 30), (2, "Bob", 25), (3, "Cara", 41)]:
            self.backend.table_insert("users", {"id": pk, "name": name, "years": age})

        # A filter returns the matching rows, by column name, in schema order
        res = self.backend.execute("from users where years > 28")
        self.assertEqual(res.columns, ["$id", "id", "name", "years"])
        self.assertEqual([r[2] for r in res.rows], ["Alice", "Cara"])

        # No match is no rows, not some other rows
        self.assertEqual(self.backend.execute("from users where years > 99").rows, [])

        # Projection and sort
        res = self.backend.execute("select name from users order by years desc")
        self.assertEqual(res.columns, ["name"])
        self.assertEqual([r[0] for r in res.rows], ["Cara", "Alice", "Bob"])

        # A query that does not compile is an error, not a procedure's result
        with self.assertRaises(BackendError) as ctx:
            self.backend.execute("select count(*) from users")
        self.assertIn("CMBQL error", str(ctx.exception))


if __name__ == "__main__":
    unittest.main()
