# datastore/gutenberg

This folder holds the GutenFree Diet demo's local data. Git ignores everything here except this file: the demo's scripts and commands create the rest. See [`demo/gutenfree/README.md`](../../demo/gutenfree/README.md).

| Path | Created by |
|---|---|
| `GUTINDEX/GUTINDEX.ALL.new` | `gf_fetch.py --gutindex` |
| `pg_catalog.csv` | `gf_fetch.py --catalog` |
| `books/<ebook>.txt`, `books/manifest.tsv` | `gf_fetch.py --count N`, `--ebook N`, or the app's download queue |
| `models/bge-small-en-v1.5-q8_0.gguf` | `gf_model.py` |
| `gutenfree.cdbarch`, `gutenfree.cdbwork/` | the database: `gutenfree ingest-index` and the other commands |

To start over, delete `gutenfree.cdbarch` and `gutenfree.cdbwork/`. The downloads can stay.
