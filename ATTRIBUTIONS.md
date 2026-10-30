# Attributions

## Authorship

db-combolite is designed and owned by Casey Annis (unklStewy).

The specification and implementation were developed with the help of
Claude, an AI assistant made by Anthropic, working through Claude Code.
Every design decision in the specification was confirmed by the owner.

## Third-party components

| Component | Use | License | Location |
|---|---|---|---|
| [Unity](https://github.com/ThrowTheSwitch/Unity) v2.6.1 | Unit-test framework, used only in tests | MIT | `db-combolite/third-party/unity/` |
| [llama.cpp](https://github.com/ggml-org/llama.cpp) b11701 (commit 01fc3aa) | CPU sentence embeddings, used only in the GutenFree Diet demo | MIT | `db-combolite/demo/third-party/llama.cpp/` (see `VENDORED.md`) |
| [ncurses](https://invisible-island.net/ncurses/) (system library) | Terminal screens, used only in the GutenFree Diet demo; linked, not vendored | X11-style (MIT) | system package `libncurses-dev` |
| [bge-small-en-v1.5](https://huggingface.co/BAAI/bge-small-en-v1.5), GGUF by [CompendiumLabs](https://huggingface.co/CompendiumLabs/bge-small-en-v1.5-gguf) | Embedding model, downloaded by the demo's `tools/gf_model.py` at setup; never committed | MIT | `db-combolite/datastore/gutenberg/models/` (git-ignored) |

Each vendored component keeps its own license file in its directory.

Book texts and the GUTINDEX catalog used by the demo come from [Project Gutenberg](https://www.gutenberg.org/), fetched from its mirrors by the demo's scripts and never committed.
