"""CMBQL lexer + diagnostics. Pure Python, no GTK, so it's unit-testable.

The query language isn't finished upstream: edit KEYWORDS / STATEMENT_STARTS below
to match the real grammar. Everything else (highlighting, squiggles, quick-fixes)
derives from these tables and `analyze()`.
"""
from __future__ import annotations

import difflib
import re
from collections import namedtuple

KEYWORDS = set("""
query procedure select from where filter join inner left outer on group aggregate by order sort
asc desc nulls first last limit offset as project unnest window asof_join and or not in is like between
true false null let if then elseif else end while do for return call begin commit rollback
savepoint release transaction tx durability pragma with case when exists distinct
insert into update set delete link to type properties unlink append timeseries table pool vector
values store model schema replace patch summary weight time reinforce series tags before prune
retention age downsample step source blob free ref create drop enclave open rotate master key
check rotation register principal tenant pubkey grant role perms perm reconcile compact reclaim
trigger peer sync since export import yield action data search cardinality enforced find apply
scan textsearch vectorsearch hybridrank traverse field match near knn bm25
""".split())
STATEMENT_STARTS = {
    "select", "from", "insert", "update", "delete", "create", "drop", "link", "unlink", "append",
    "call", "explain", "begin", "commit", "rollback", "savepoint", "release", "pragma", "with",
    "query", "procedure", "let", "if", "while", "for", "return", "rotate", "check", "register",
    "grant", "reconcile", "compact", "reclaim", "replace", "patch", "reinforce", "prune",
    "downsample", "free", "peer", "sync", "export", "import", "yield", "find", "scan", "search",
    "traverse", "textsearch", "vectorsearch", "filter", "window", "sort", "limit", "project"
}
FUNCTIONS = set("""
abs round floor ceil sqrt pow mod length lower upper coalesce now count sum avg min max
sin cos tan asin acos atan atan2 sinh cosh tanh degrees radians hypot hypot3 haversine
bbox_contains bearing destination_lat destination_lon stddev variance
""".split())
CLAUSE_END = {"limit", "order", "sort", "group", "offset", "having", "where", "filter", "window", "yield", "by", "on"}
NAME_AFTER = {
    "from", "join", "into", "update", "table", "pool", "collection", "series", "timeseries",
    "vector", "link", "unlink", "to", "call", "store", "model", "schema", "enclave",
    "principal", "tenant", "peer", "trigger", "scan", "search", "source"
}

Token = namedtuple("Token", "kind start end text bad")
# fix = (start, end, replacement) or None; sev = "error" | "warning" | "info"
Diag = namedtuple("Diag", "start end sev msg fix")

_NUM = re.compile(r"\d+(\.\d+)?([eE][+-]?\d+)?")
_WORD = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_PARAM = re.compile(r"[$@:][A-Za-z_][A-Za-z0-9_]*")
_OPS2 = ("<=", ">=", "!=", "<>", "==", "->", "<-", "||", "|>")
_OPS1 = "=<>+-*/%|&~"
_PUNCT = "(),;.[]{}"


def tokenize(text: str) -> list[Token]:
    toks, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c.isspace():
            i += 1
        elif text.startswith("--", i) or c == "#":
            j = text.find("\n", i)
            j = n if j < 0 else j
            toks.append(Token("comment", i, j, text[i:j], False))
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            end = n if j < 0 else j + 2
            toks.append(Token("comment", i, end, text[i:end], j < 0))
            i = end
        elif c in "'\"":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j, n)
            ok = j < n and text[j] == c
            end = j + 1 if ok else j
            toks.append(Token("str", i, end, text[i:end], not ok))
            i = end
        elif c.isdigit() or (c == "." and text[i + 1:i + 2].isdigit()):
            m = _NUM.match(text, i)
            end, bad = (m.end() if m else i + 1), False
            if end < n and (text[end].isalpha() or text[end] == "_"):
                while end < n and (text[end].isalnum() or text[end] == "_"):
                    end += 1
                bad = True
            toks.append(Token("error" if bad else "num", i, end, text[i:end], bad))
            i = end
        elif (m := _PARAM.match(text, i)) and c != ":":
            toks.append(Token("param", i, m.end(), m.group(), False))
            i = m.end()
        elif (m := _WORD.match(text, i)):
            w = m.group()
            low = w.lower()
            kind = "fn" if low in FUNCTIONS and re.compile(r"\s*\(").match(text, m.end()) else ("kw" if low in KEYWORDS else "ident")
            toks.append(Token(kind, i, m.end(), w, False))
            i = m.end()
        elif text[i:i + 2] in _OPS2:
            toks.append(Token("op", i, i + 2, text[i:i + 2], False))
            i += 2
        elif c in _OPS1:
            toks.append(Token("op", i, i + 1, c, False))
            i += 1
        elif c in _PUNCT or c == ":":
            toks.append(Token("punct", i, i + 1, c, False))
            i += 1
        else:
            toks.append(Token("error", i, i + 1, c, True))
            i += 1
    return toks


def _close(word, options):
    m = difflib.get_close_matches(word, list(options), n=1, cutoff=0.8)
    return m[0] if m and m[0] != word else None


def analyze(text: str, names=()) -> list[Diag]:
    """Lexer-level and shallow structural checks. `names` = known store names."""
    toks = tokenize(text)
    out: list[Diag] = []
    known = {n.lower(): n for n in names}

    for t in toks:
        if t.kind == "comment" and t.bad:
            out.append(Diag(t.start, t.end, "error", "Unterminated block comment", (t.end, t.end, "*/")))
        elif t.kind == "str" and t.bad:
            out.append(Diag(t.start, t.end, "error", "Unterminated string literal", (t.end, t.end, text[t.start])))
        elif t.kind == "error":
            msg = f"Malformed number '{t.text}'" if t.text[0].isdigit() else f"Unexpected character '{t.text}'"
            out.append(Diag(t.start, t.end, "error", msg, (t.start, t.end, "") if not t.text[0].isdigit() else None))

    pairs, stack = {"(": ")", "[": "]", "{": "}"}, []
    for t in toks:
        if t.kind != "punct":
            continue
        if t.text in pairs:
            stack.append(t)
        elif t.text in pairs.values():
            if stack and pairs[stack[-1].text] == t.text:
                stack.pop()
            else:
                out.append(Diag(t.start, t.end, "error", f"Unmatched '{t.text}'", (t.start, t.end, "")))
    for t in stack:
        out.append(Diag(t.start, t.end, "error", f"'{t.text}' is never closed", (len(text), len(text), pairs[t.text])))

    sig, stmts = [t for t in toks if t.kind not in ("comment",)], [[]]
    for t in sig:
        if t.kind == "punct" and t.text == ";":
            stmts.append([])
        else:
            stmts[-1].append(t)

    for st in (s for s in stmts if s):
        first = st[0]
        low = first.text.lower()
        if first.kind == "kw":
            if low not in STATEMENT_STARTS:
                out.append(Diag(first.start, first.end, "error", f"A statement can't start with '{first.text}'", None))
        elif first.kind == "ident":
            m = _close(low, STATEMENT_STARTS)
            out.append(Diag(first.start, first.end, "error",
                            f"Unknown keyword '{first.text}'" + (f" — did you mean '{m}'?" if m else ""),
                            (first.start, first.end, m) if m else None))
        elif first.kind not in ("error",):
            out.append(Diag(first.start, first.end, "error", "Expected a statement keyword (e.g. from, select)", None))

        for i, t in enumerate(st):
            prev = st[i - 1] if i else None
            nxt = st[i + 1] if i + 1 < len(st) else None
            pl = prev.text.lower() if prev and prev.kind == "kw" else ""
            if t.kind == "ident" and i > 0:
                lw = t.text.lower()
                if pl in NAME_AFTER:
                    if known and lw not in known:
                        m = _close(lw, known)
                        out.append(Diag(t.start, t.end, "warning",
                                        f"No store named '{t.text}'" + (f" — did you mean '{known[m]}'?" if m else ""),
                                        (t.start, t.end, known[m]) if m else None))
                elif prev and prev.text not in (".", ":") and len(lw) >= 4 and lw not in known:
                    m = _close(lw, KEYWORDS)
                    if m:
                        out.append(Diag(t.start, t.end, "warning", f"'{t.text}' looks like the keyword '{m}'", (t.start, t.end, m)))
            if (t.kind == "kw" and prev and prev.kind == "kw" and prev.text.lower() == t.text.lower()
                    and t.text.lower() in ("where", "from", "select", "limit", "by", "and", "or", "group", "order")):
                out.append(Diag(t.start, t.end, "warning", f"Repeated '{t.text}'", (prev.end, t.end, "")))
            if t.kind == "op" and t.text in ("=", "==", "!=", "<>") and nxt and nxt.kind == "kw" and nxt.text.lower() == "null":
                neg = t.text in ("!=", "<>")
                out.append(Diag(t.start, nxt.end, "warning", "NULL never equals anything — use 'is null'",
                                (t.start, nxt.end, "is not null" if neg else "is null")))
            if t.kind == "kw" and t.text.lower() in ("where", "and", "or", "not", "by", "having"):
                if nxt is None or (nxt.kind == "kw" and nxt.text.lower() in CLAUSE_END):
                    out.append(Diag(t.start, t.end, "error", f"Expected an expression after '{t.text}'", None))

        if low in ("from", "select") and not any(t.kind in ("kw", "fn") and t.text.lower() in ("limit", "count") for t in st):
            end = st[-1].end
            out.append(Diag(first.start, first.end, "info", "No limit — this scans the whole store", (end, end, " limit 100")))

    return sorted(out, key=lambda d: (d.start, d.end))


def apply_fix(text: str, d: Diag) -> str:
    s, e, r = d.fix
    return text[:s] + r + text[e:]


def apply_all(text: str, diags) -> str:
    """Apply every non-info fix, last-to-first, skipping overlaps."""
    taken, edge = [], len(text) + 1
    for d in sorted((d for d in diags if d.fix and d.sev != "info"), key=lambda d: -d.fix[0]):
        s, e, _ = d.fix
        if e <= edge:
            taken.append(d)
            edge = s
    for d in taken:
        text = apply_fix(text, d)
    return text
