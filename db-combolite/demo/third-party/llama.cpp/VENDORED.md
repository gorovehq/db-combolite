# llama.cpp, vendored for the GutenFree Diet demo

Used only by the demo (`demo/gutenfree/gf_embed.c`), for CPU sentence embeddings. The engine never links it. It is built only with `-DCDB_BUILD_DEMO=ON`.

| | |
|---|---|
| Upstream | https://github.com/ggml-org/llama.cpp |
| Licence | MIT (`LICENSE`) |
| Release | `b11701`, published 2026-10-16 23:20 UTC |
| Commit | `01fc3aae846caa644f0cee360f108d4c48e4aaa1` |
| Tarball | https://github.com/ggml-org/llama.cpp/archive/refs/tags/b11701.tar.gz |
| Tarball SHA-256 | `ecfebd1465b61524ea084eca81cdd1439ee219dfc5e298e5c552336cee706eb7` |
| Vendored | 2026-10-30 |

**Pin rule:** the newest tagged release that is at least two weeks old. This follows the lesson of xz 5.6.0/5.6.1, where a malicious release was caught within weeks of publication.

**Left out of the tarball** (not needed to build the library): `models/`, `docs/`, `tools/`, `tests/`, `examples/`, `benches/`, `pocs/`, `media/`, `gguf-py/`, `conversion/`, `scripts/`, `requirements/`, `skills/`, `ci/`, `.github/`, `.devops/`, `grammars/`, `app/`, `common/` and `convert_*.py`.

**Removed after extraction:**
- `CLAUDE.md`, `AGENTS.md`, `.gemini/`, `.pi/`: upstream's instructions for coding agents, which must not act as instructions inside this repository.
- `.gitignore`, so that git never silently skips a vendored file.

**To update:** download a newer tag that meets the pin rule, extract it the same way, re-apply the removals, and update this table.
