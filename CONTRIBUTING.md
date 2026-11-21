# Contributing to db-combolite

`db-combolite` is a multi-package workspace for the Combolite database engine. Contributions
(bug fixes, features, docs, tests) are welcome and go through the **same review pipeline** as
internal work: tracked by an issue, reviewed, and never merged unreviewed.

> The authoritative, complete engineering ruleset is [`CLAUDE.md`](CLAUDE.md) at this repo root.
> Where this file and `CLAUDE.md` differ, `CLAUDE.md` wins — please read it before a non-trivial
> change. Each package may also carry its own `CONTRIBUTING.md` / docs with package-specific detail
> (e.g. [`combolite-core/CONTRIBUTING.md`](combolite-core/CONTRIBUTING.md)).

## Packages

| Package | What |
|---|---|
| `combolite-core` | The C99 database engine (crash-safe, no-heap, multi-model). |
| `combolite-accelerator` | Optional CUDA / Vulkan accelerators (`combolite_accel`); never part of the core. |
| `combolite-tests` | Cross-package / integration tests. |
| `combolite-studio` | Studio tooling. |
| `combolite-demo` | The GutenFree Diet demo. |

## Ground rules (non-negotiable)

- **C99 only** in the engine; builds are **warnings-as-errors** (`-Wconversion -Wsign-conversion
  -Wvla`, no VLAs, explicit casts). **No heap in the core** — caller-owned buffers + optional host
  `alloc`/`free` hooks; readers return views; respect slot-pointer lifetime. Explicit byte order
  (`cdb_endian.h`); canonical encoding; fail closed (`cdb_status`). **Never hand-roll crypto.**
  Treat all on-disk / wire / query input as untrusted. (Full detail: [`CLAUDE.md`](CLAUDE.md).)

## Development environment

```bash
# Clone with the vendored submodules (Mbed TLS, xz, Unity).
git clone --recurse-submodules https://github.com/gorovehq/db-combolite.git && cd db-combolite
# (already cloned?)  git submodule update --init --recursive

# Canonical test preset — ASan + UBSan (+ CUDA/Vulkan accel + studio + demo):
cmake --preset core-both-debug && cmake --build --preset core-both-debug
ctest --preset core-both-debug

# CPU-only (no CUDA/Vulkan SDKs):
cmake --preset core-cpu-debug && cmake --build --preset core-cpu-debug && ctest --preset core-cpu-debug
```

Reference docs live per package (`combolite-core/docs/dev`, `combolite-core/docs/spec`) and in each
package's README.

## Code style

- C formatting is enforced by **`.clang-format`** (4-space, Linux brace style, 100-col) — shared
  across packages. Run `clang-format -i` on C files you touch; CI checks changed lines. `.editorconfig`
  covers other file types. Every public symbol carries a doc comment.

## Tests (definition of "done")

- Every change ships tests. **Every bug fix adds a regression test that reproduces the defect first**
  (red→green). Untrusted-input / state-mutating code ships negative/abuse-path tests; crash-safety via
  the fault devices. The suite must be **ASan + UBSan clean**; coverage must not regress. Scope CI to
  affected packages, but run security scans across the workspace.

## Git workflow

- Branch off `main`; **underscore type prefixes**: `feat_`, `fix_`, `test_`, `docs_`, `refactor_`,
  `build_`, `merge_`. **Conventional Commits**, imperative, ≤72-char subject; atomic; **signed**.
- Use your GitHub **noreply** email. **No AI-assistant attribution** in commits/PRs — attribution
  lives only in [`ATTRIBUTIONS.md`](ATTRIBUTIONS.md).
- Merge to `main` is **`--no-ff`**; **merged branches are kept** (audit trail). No direct pushes / no
  force-push to `main`.
- **Submodules:** bump vendored third-party deliberately in their own `build_`/`chore_` commit; never
  mix a submodule bump with a feature change.

## Pull requests

- Every change to `main` goes through a PR referencing its issue (`Fixes #N`); use the PR template.
  At least one approving review (security-focused for security-relevant changes). All CI gates green.
  Keep PRs small and single-purpose. Note which package(s) are affected.

## Reporting

- File issues with the templates (bug / feature). **Security vulnerabilities:** do **not** open a
  public issue — use the private advisory flow (see [`SECURITY.md`](SECURITY.md)).
- Contributors are acknowledged in [`ATTRIBUTIONS.md`](ATTRIBUTIONS.md). By contributing you agree
  your contributions are licensed under the repository's license.
