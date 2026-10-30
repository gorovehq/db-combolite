# data/

This folder holds the GutenFree Diet demo's local data. Git ignores everything here except this file: the scripts and commands in the folder above create the rest. See [`../README.md`](../README.md).

| Path | Created by (run from `demo/gutenfree/`) |
|---|---|
| `GUTINDEX.ALL` | `python3 gf_fetch.py --gutindex` |
| `pg_catalog.csv` | `python3 gf_fetch.py --catalog` |
| `books/<ebook>.txt`, `books/manifest.tsv` | `python3 gf_fetch.py --count N` or `--ebook N`, or the app's download queue |
| `models/bge-small-en-v1.5-q8_0.gguf` | `python3 gf_model.py` |
| `gutenfree.cdbarch`, `gutenfree.cdbwork/` | the database: `./gutenfree ingest-index` and the other commands |

To start over, delete `gutenfree.cdbarch` and `gutenfree.cdbwork/`. The downloads can stay.
