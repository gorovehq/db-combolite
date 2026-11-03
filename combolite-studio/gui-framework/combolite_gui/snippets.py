"""Tool-rail palette for the query console. Edit freely: each snippet is
(label, text, mode, tooltip). `|` marks where the cursor lands.
mode: "stmt" = new statement at end · "inline" = at cursor · "prefix" = start of query."""
from collections import namedtuple

Cat = namedtuple("Cat", "key glyph label tip snippets")

CATS = [
    Cat("query", "Q", "Query", "Statements", [
        ("Select", "from |books\nlimit 100", "stmt", "Read rows from a store"),
        ("Insert", "insert into books (title, year)\nvalues ('|Untitled', 1900)", "stmt", "Insert a row"),
        ("Update", "update books\nset year = |1900\nwhere id = 1", "stmt", "Update rows"),
        ("Delete", "delete from books\nwhere id = |1", "stmt", "Delete rows"),
        ("Full-text", "from catalog\nmatch '|river lantern'\nlimit 20", "stmt", "BM25 search a document pool"),
        ("Nearest", "from book_embeddings\nnear '|vec-00042'\nlimit 10", "stmt", "k-NN vector search"),
        ("Link", "link written_by\nfrom '|book-00001' to 'author-0001'", "stmt", "Create a link"),
        ("Unlink", "unlink written_by\nfrom '|book-00001' to 'author-0001'", "stmt", "Remove a link"),
        ("Series", "from reads_per_min\nwhere ts > now() - 3600\nlimit |100", "stmt", "Recent time-series points"),
    ]),
    Cat("clause", "{}", "Clause", "Clauses", [
        ("where", "where |", "inline", ""), ("order by", "order by | desc", "inline", ""),
        ("group by", "group by |", "inline", ""), ("having", "having |", "inline", ""),
        ("limit", "limit |100", "inline", ""), ("offset", "offset |0", "inline", ""),
        ("join", "join | on ", "inline", ""), ("as", "as |", "inline", "Alias"),
    ]),
    Cat("logic", "?", "Logic", "Conditionals & comparisons", [
        ("and", "and ", "inline", ""), ("or", "or ", "inline", ""), ("not", "not ", "inline", ""),
        ("=", "= ", "inline", ""), ("!=", "!= ", "inline", ""), ("<", "< ", "inline", ""),
        ("<=", "<= ", "inline", ""), (">", "> ", "inline", ""), (">=", ">= ", "inline", ""),
        ("in ( )", "in (|)", "inline", "Membership"), ("between", "between | and ", "inline", "Range"),
        ("like", "like '%|%'", "inline", "Pattern match"),
        ("is null", "is null", "inline", "NULL test"), ("is not null", "is not null", "inline", ""),
        ("case", "case\n  when | then \n  else \nend", "inline", "Conditional expression"),
        ("exists", "exists (|)", "inline", ""),
    ]),
    Cat("math", "Σ", "Math", "Operators, math & aggregates", [
        ("+", "+ ", "inline", ""), ("−", "- ", "inline", ""), ("×", "* ", "inline", ""),
        ("÷", "/ ", "inline", ""), ("mod %", "% ", "inline", "Modulo"),
        ("abs", "abs(|)", "inline", ""), ("round", "round(|, 2)", "inline", ""), ("floor", "floor(|)", "inline", ""),
        ("ceil", "ceil(|)", "inline", ""), ("sqrt", "sqrt(|)", "inline", ""), ("pow", "pow(|, 2)", "inline", ""),
        ("count", "count(*)", "inline", "Aggregate"), ("sum", "sum(|)", "inline", "Aggregate"),
        ("avg", "avg(|)", "inline", "Aggregate"), ("min", "min(|)", "inline", "Aggregate"),
        ("max", "max(|)", "inline", "Aggregate"), ("coalesce", "coalesce(|, 0)", "inline", "First non-null"),
        ("now()", "now()", "inline", "Current time"),
    ]),
    Cat("directive", "#", "Direct.", "Directives & transactions", [
        ("explain", "explain ", "prefix", "Show the query plan"),
        ("begin", "begin;", "stmt", "Start a transaction"), ("commit", "commit;", "stmt", ""),
        ("rollback", "rollback;", "stmt", ""), ("pragma", "pragma |", "stmt", "Engine setting"),
        ("-- note", "-- |", "inline", "Comment"), ("$param", "$|name", "inline", "Procedure parameter"),
    ]),
    Cat("procs", "ƒ", "Procs", "Stored procedures", []),
]
