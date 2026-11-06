# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository. Reference material lives in `db-combolite/docs/dev/`; read the file that matches the task:

| File | Read it when |
|---|---|
| `docs/dev/milestones.md` | starting, finishing or asked about a milestone; the project's status and open items |
| `docs/dev/code-layout.md` | working inside a module: the engine layers, the accelerator, slots, nibblets, test tooling, directory intent |
| `docs/dev/project.md` | making a design decision: what db-combolite is, the hard constraints in full, approved dependencies, its role in the governance framework |

## Project status

The spec in `db-combolite/docs/spec/` is complete; start with its `README.md`, which lists every section, its dependencies and what is still open. Every spec section records its confirmed decisions; code follows the spec, and spec section numbers are cited in code comments. Implementation in C99 follows the milestone plan in `docs/dev/milestones.md`. The user reviews each milestone before the next one starts.

## Commands

All commands run from `db-combolite/`:

```sh
cmake -S . -B build                          # configure (Debug by default: ASan and UBSan on)
cmake --build build -j                       # build the library and the tests
ctest --test-dir build --output-on-failure   # run every test
ctest --test-dir build -R test_value         # run one test executable
./build/tests/test_value                     # run one executable directly (Unity output)
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release   # optimised build, sanitizers off
cmake --preset noxz                          # mcu-profile test build (spec 17): Debug, xz off, in build-noxz/ (paused until after 1.0)
cmake -S . -B build-cuda -DCDB_ACCEL_CUDA=ON -DCDB_ACCEL_VULKAN=ON # the build to test in: host tests plus combolite_accel with both backends, their device tests and bench_accel
python3 tools/gen_spirv.py                   # after editing accel/vulkan/shaders: rewrites the checked-in SPIR-V (needs glslc)
```

The demo is built separately; see `demo/gutenfree/README.md`.

## Git workflow

- **Never commit directly to `main`.** Every piece of work goes on its own branch, named with a type prefix:

  | Prefix | Use |
  |---|---|
  | `feat_` | new functionality, such as a milestone (`feat_m4-pages-anchor-map`) |
  | `fix_` | bug fixes |
  | `test_` | test-only changes |
  | `docs_` | spec and documentation changes |
  | `merge_` | integration branches that bring other branches together |
  | `refactor_` | restructuring that does not change behaviour |
  | `build_` | build system, CMake, CI and toolchain changes |

- **Branch names** take the form `<prefix><short-kebab-description>`.
- **Commit and merge only when the user asks.** Merging into `main` is the user's decision.
- **Merges into `main` use `--no-ff`,** so every branch shows up in the history as a single unit.
- **Keep merged branches.** Don't delete them; they are the audit trail of how development happened.
- **No AI attribution in commits.** Don't add `Co-Authored-By: Claude` trailers or similar lines to commit messages or PR descriptions. Attribution lives only in `ATTRIBUTIONS.md` at the repo root. Add each new third-party dependency to the table there.

## Code rules

These were agreed with the user:

- **C99 only.** `CMAKE_C_EXTENSIONS` is off.
- **Warnings are errors.** The flags include `-Wconversion`, `-Wsign-conversion` and `-Wvla`, so write explicit casts and never use VLAs.
- **No heap allocation in the core.** Memory comes from the caller: buffers, and the environment's optional `alloc`/`free` hooks (`cdb_env.h`), which only the host environment backs with malloc. Anything that grows must also work from caller memory alone, so microcontroller builds can use static pools. Readers return views into caller-owned buffers.
- **Build profiles (spec 17).** `CDB_PROFILE=host` (the default) or `mcu`, in `src/cdb_profile.h`. Both read and write the same files; only a process's limits differ. On host, query working memory grows through the environment's `alloc` hooks up to `cdb_db_config.query_mem`, so size host limits for speed. On mcu, everything stays in caller memory. A profile limit that is reached is `CDB_E_OUT_OF_BUDGET` in both. **Until after 1.0, test in `build-cuda` only** (host profile, Debug with sanitizers, configured with `-DCDB_ACCEL_CUDA=ON -DCDB_ACCEL_VULKAN=ON`): it runs every host test plus the accelerator's, so the CPU-only `build` is not run separately, to save time and tokens. Microcontroller work and the `build-noxz` (mcu) suite are paused until after 1.0: the mcu profile's code stays and must keep compiling in principle, but its suite is not run.
- **Bindings mirror structs.** The studio's `binding.py` mirrors `cdb_db_config`, `cdb_exec_row` and others, and checks their sizes at load (`cdb_db_config_size`, `cdb_exec_row_size`, ...). Add new config fields at the end, and update `binding.py` with any struct change.
- **Fallible functions return `cdb_status`**, defined in `include/combolite/cdb_common.h`. The value writer's errors are sticky.
- **Explicit byte order.** Every multi-byte on-disk field goes through `src/cdb_endian.h`, using the file's declared byte order. Never cast raw bytes to integers.
- **Canonical encoding.** Writers produce the one canonical form, and validators reject anything else (spec 14.6.5).
- **Tests use Unity**, vendored in `third-party/unity/` (v2.6.1, MIT). Add a test executable with `cdb_add_test(<name>)` in `tests/CMakeLists.txt`.

Plus four rules moved up from the code layout, because most changes need them:

- **Locking.** Every public data call holds the database's re-entrant lock (`cdb_db_lock`) for its whole run, so worker-pool tasks never overlap an application call. A new public call takes it too: wrap an unchanged static body.
- **Crash sweeps.** Every new write sequence needs a sweep: count the workload's mutating operations, crash at each one in every mode of the fault devices (`src/cdb_faultdev.*`, `src/cdb_faultenv.*`), reboot, and check what recovery sees. The reorder mode is what catches a missing barrier.
- **The apply hook must never fail for logical reasons,** because it runs again during recovery.
- **Slot pointers** are valid until the next open of the same model outside a transaction or operation scope. Code that holds one store while opening another of the same model wraps the work in `cdb_db_op_begin`/`cdb_db_op_end`.

## Hard constraints

These drive every design decision; the full text is in `docs/dev/project.md`.

1. **Serverless, portable and embeddable.** A library; one file at rest; no daemon. Processes coordinate only through the files, each through its in-process coordinator. Single host only.
2. **Microcontrollers are a long-term goal.** No MCU targets or HAL yet, but no format decision that would block a constrained implementation. Microcontroller work is paused until after 1.0.
3. **Scales up to production.** Crash safety, integrity checks, concurrent readers.
4. **Not a server RDBMS competitor.** Turn down features that only make sense for one.
5. **Clean room.** Never vendor, port, wrap or copy another engine's code, and stay format-incompatible with them. Use only the approved dependencies in `docs/dev/project.md`, and never hand-roll cryptography.

Keep governance-specific logic out of the core engine: governance is a consumer.

## How spec work proceeds

Write the spec one section at a time, and stop before each section. First explain what the section covers and which decisions it involves, along with their trade-offs. Then ask the user for direction. Don't write design choices into the spec that the user hasn't confirmed. Use placeholders when the user is still undecided.
