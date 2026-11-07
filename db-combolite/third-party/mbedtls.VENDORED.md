# Vendored Mbed TLS (libmbedcrypto)

- **Release:** Mbed TLS 3.6.2 (LTS), released 2024-10-14.
- **Source:** `https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2`
- **SHA-256 of the tarball:** `8b54fb9bcf4d5a7078028e0520acddefb7900b3e66fec7f7175bb5b7d85ccdca`
- **License:** Apache License 2.0 / GPL-2.0-or-later; see `LICENSE`.

## What is kept

- **Kept:**
  - `CMakeLists.txt` and `cmake/`, Mbed TLS's own build;
  - `include/` and `library/`, the core cryptographic library (`libmbedcrypto`);
  - `3rdparty/` (Everest Curve25519 and p256-m);
  - `framework/` (internal CMake modules);
  - `LICENSE`, `README.md`, `SECURITY.md`, `BUGS.md`, and `ChangeLog`.
- **Left out:**
  - `programs/` (command-line sample tools);
  - `tests/` (Mbed TLS's internal test suites);
  - `doxygen/` and `docs/`;
  - `visualc/`.

## How it is built

`db-combolite/CMakeLists.txt` configures and adds this directory with `EXCLUDE_FROM_ALL` and links `mbedcrypto` into `combolite`. The following options are enforced:

- `ENABLE_PROGRAMS=OFF`
- `ENABLE_TESTING=OFF`
- `MBEDTLS_FATAL_WARNINGS=OFF`
- `INSTALL_MBEDTLS_HEADERS=OFF`

## Since 2026-11-07: a git submodule

This library is now the submodule `third-party/mbedtls`, pinned to the same release tag, shallow; the source is the upstream git tree at that tag rather than the release tarball, so the tarball SHA-256 above no longer describes it. Its own tests are kept out of the build.
