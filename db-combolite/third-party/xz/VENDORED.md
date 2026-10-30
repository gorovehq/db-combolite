# Vendored XZ Utils (liblzma only)

- **Release:** xz 5.8.4, released 2026-09-09.
- **Source:** `https://github.com/tukaani-project/xz/releases/download/v5.8.4/xz-5.8.4.tar.gz`
- **SHA-256 of the tarball:** `0014c7886930454fe8bd4228665b51af55eeae560ea135c9c4cd33f55b2591d9`
- **Signature:** `xz-5.8.4.tar.gz.sig` verified good on 2026-10-30. It was signed by Lasse Collin's release key, fingerprint `3690 C240 CE51 B467 0D30 AD1C 38EE 757D 6918 4620`, fetched from `https://tukaani.org/misc/lasse_collin_pubkey.txt`.
- **Never use 5.6.0 or 5.6.1** (CVE-2024-3094).

## What is kept

- **Kept, unmodified:**
  - `CMakeLists.txt` and `cmake/`, xz's own build;
  - `src/liblzma/`;
  - `src/common/`, the tuklib helpers liblzma uses;
  - `COPYING`, `COPYING.0BSD`, `AUTHORS`, `README` and `NEWS`.

  All of these are under the BSD Zero Clause License (0BSD); see `COPYING`.
- **Left out:**
  - the command-line tools and scripts;
  - `lib/` (GNU getopt, LGPL), which only the tools use;
  - the tests, docs, translations and the Autotools build.

## How it is built

`db-combolite/CMakeLists.txt` adds this directory with `EXCLUDE_FROM_ALL` when `CDB_WITH_XZ` is on (the default). It sets xz's options first:

- **Encoders and decoders:** LZMA1 and LZMA2 only. The LZMA2 encoder requires LZMA1.
- **Match finders:** hc3, hc4 and bt4.
- **Checks:** CRC32 only.
- **Off:** threads, translations, docs and every tool.

To update, download a new release, verify its signature against the same key, replace the kept files, update this note and `ATTRIBUTIONS.md`, and run the whole test suite.
