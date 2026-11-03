import unittest
from combolite_gui.cmbql import tokenize, analyze, apply_all, KEYWORDS, STATEMENT_STARTS, FUNCTIONS


class TestCMBQL(unittest.TestCase):
    def test_keywords_parity(self):
        for kw in ["query", "procedure", "select", "from", "scan", "filter", "project",
                   "enclave", "rotate", "peer", "sync", "reclaim", "compact"]:
            self.assertIn(kw, KEYWORDS)

    def test_statement_starts(self):
        for start in ["select", "from", "scan", "insert", "update", "delete", "create",
                      "drop", "link", "unlink", "append", "rotate", "grant", "peer"]:
            self.assertIn(start, STATEMENT_STARTS)

    def test_flux_pipeline_tokenization(self):
        query = 'Scan("books") |> filter(score >= 0.8) |> sort(score DESC) |> limit(10);'
        tokens = tokenize(query)
        kinds = [(t.kind, t.text) for t in tokens]
        self.assertIn(('op', '|>'), kinds)
        self.assertIn(('kw', 'filter'), kinds)
        self.assertIn(('kw', 'sort'), kinds)
        self.assertIn(('kw', 'limit'), kinds)

    def test_functions(self):
        for fn in ["sin", "cos", "haversine", "hypot", "round", "abs"]:
            self.assertIn(fn, FUNCTIONS)
        toks = tokenize("sin(angle) + haversine(lat1, lon1, lat2, lon2)")
        self.assertEqual(toks[0].kind, "fn")
        self.assertEqual(toks[0].text, "sin")

    def test_diagnostics_unmatched_brackets(self):
        diags = analyze("Scan('books' |> filter(score > 10")
        errors = [d for d in diags if d.sev == "error"]
        self.assertTrue(len(errors) > 0)

    def test_diagnostics_unknown_keyword_suggestion(self):
        diags = analyze("selec * from books;")
        errors = [d for d in diags if "did you mean 'select'" in d.msg]
        self.assertTrue(len(errors) > 0)


if __name__ == "__main__":
    unittest.main()
