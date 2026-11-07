# combolite

combolite is an embeddable, serverless multi-model database library in C99. At rest a database is a single archive file. It stores several data models natively:

- **Relational tables**, row-oriented, with constraints, secondary indexes and schema changes.
- **Document pools**, with revision history, patches, schema validation and full-text search (BM25).
- **Vector collections**, with exact search, an optional neighbour graph for approximate search, and binary (1-bit) vectors.
- **Links** between entries, with cardinality and delete rules.
- **Time series**, with bit-packed column blocks and late-arrival modes.

It is serverless: a library, a single file at rest, and no daemon. It is built for crash safety. The core never allocates on the heap; all memory comes from the caller, enabling microcontroller builds. The file format, storage engine and query layer are a clean-room design.

> **Status: active development.** The specification is complete and implementation follows a milestone plan. Not built yet: CMBQL query core, column-oriented tables, and cross-store transactions.

---

## Repository Layout

This umbrella repository organizes combolite into modular components managed as git submodules:

| Component | Path | Description |
|---|---|---|
| **Core Engine** | [`combolite-core/`](combolite-core/) | C99 database engine, public headers (`include/combolite/`), specification (`docs/spec/`), developer documentation, and core unit/crash tests. Fully functional CPU-only fallback. |
| **Hardware Accelerator** | [`combolite-accelerator/`](combolite-accelerator/) | Optional CUDA and Vulkan hardware acceleration library (`combolite_accel`) for high-throughput vector and sorting operations. |
| **Combolite Studio** | [`combolite-studio/`](combolite-studio/) | Native GTK 4 / libadwaita desktop workbench and CMBQL query runner. |
| **Demo Application** | [`combolite-demo/`](combolite-demo/) | **GutenFree Diet**: ncurses e-reader and ingest pipeline exercising every data model against Project Gutenberg books. |
| **Test Framework** | [`combolite-tests/`](combolite-tests/) | Unified test framework (vendored Unity, CMake helpers, and runner scripts). |

---

## Prerequisites

Ensure the following prerequisites are installed for the components you intend to build:

| Component | Requirements |
|---|---|
| **All / Core** | C99 compiler (`gcc` >= 9 or `clang` >= 11), CMake >= 3.20, `git` |
| **CUDA Accelerator** | NVIDIA CUDA Toolkit >= 11.8 (with `nvcc` and `cublas`), NVIDIA GPU (Turing, Ampere, Ada, Blackwell: sm_75, 86, 89, 120) |
| **Vulkan Accelerator** | Vulkan SDK (Vulkan loader >= 1.3, headers, and `glslc` compiler) |
| **GutenFree Demo** | `ncurses` development libraries (`libncurses-dev`) |
| **Combolite Studio** | Python >= 3.9, GTK 4, libadwaita >= 1.2, PyGObject (`python3-gi`, `gir1.2-adw-1`) |

---

## Cloning

Because the repository uses submodules (including nested third-party dependencies like Mbed TLS and llama.cpp), clone with `--recursive`:

```sh
git clone --recursive https://github.com/gorovehq/combolite.git
cd combolite
```

If you already cloned without `--recursive`, initialize and fetch submodules:

```sh
git submodule update --init --recursive --depth 1
```

---

## Building and Testing

All builds use [CMake Presets](CMakePresets.json) from the repository root:

### Common Workflow

```sh
# Configure and build the core engine with CUDA and Vulkan accelerator tests (Debug + ASan/UBSan)
cmake --preset core-both-debug
cmake --build --preset core-both-debug -j

# Run the complete test suite (67 tests)
ctest --preset core-both-debug -j4

# Run one test executable
./build-core-both-debug/combolite-core/tests/test_coord
```

### CMake Presets Reference

| Preset Name | Description | Output Directory |
|---|---|---|
| `core-both-debug` | Core + CUDA + Vulkan accelerators, Debug build with ASan/UBSan (standard test baseline) | `build-core-both-debug/` |
| `core-both-release` | Core + CUDA + Vulkan accelerators, Release build | `build-core-both-release/` |
| `core-cpu-debug` | Core engine CPU-only fallback, Debug build with sanitizers | `build-core-cpu-debug/` |
| `core-cpu-release` | Core engine CPU-only fallback, Release build | `build-core-cpu-release/` |
| `core-cuda-debug` | Core + CUDA accelerator, Debug build | `build-core-cuda-debug/` |
| `core-cuda-release` | Core + CUDA accelerator, Release build | `build-core-cuda-release/` |
| `core-vulkan-debug` | Core + Vulkan accelerator, Debug build | `build-core-vulkan-debug/` |
| `core-vulkan-release` | Core + Vulkan accelerator, Release build | `build-core-vulkan-release/` |
| `gutenfree-both` | GutenFree Diet demo with both CUDA & Vulkan accelerators | `build-gutenfree-both/` |
| `gutenfree-cuda` | GutenFree Diet demo with CUDA acceleration | `build-gutenfree-cuda/` |
| `gutenfree-vulkan` | GutenFree Diet demo with Vulkan acceleration | `build-gutenfree-vulkan/` |
| `gutenfree-cpu` | GutenFree Diet demo CPU fallback | `build-gutenfree-cpu/` |
| `studio-both` | Studio shared library with CUDA & Vulkan accelerators | `build-studio-both/` |
| `studio-cuda` | Studio shared library with CUDA accelerator | `build-studio-cuda/` |
| `studio-vulkan` | Studio shared library with Vulkan accelerator | `build-studio-vulkan/` |
| `studio-cpu` | Studio shared library CPU fallback | `build-studio-cpu/` |
| `noxz` | Microcontroller (MCU) profile test build (paused until after 1.0) | `build-noxz/` |

---

## Running Studio & Demo

### Combolite Studio

Build the Studio backend library, then run or test the Python application:

```sh
cmake --preset studio-cpu
cmake --build --preset studio-cpu -j

# Run Studio Python test suite
python3 -m unittest discover -s combolite-studio/tests

# Launch Combolite Studio
python3 -m combolite-studio.combolite_gui
```

### GutenFree Diet Demo

Build and launch the e-reader or ingest pipeline:

```sh
cmake --preset gutenfree-cpu
cmake --build --preset gutenfree-cpu -j

# Inspect CLI options
./build-gutenfree-cpu/combolite-demo/gutenfree --help
```

---

## Licence and Attributions

MIT: see [`LICENSE`](LICENSE) or component submodules. Third-party components and data sources are documented in [`ATTRIBUTIONS.md`](ATTRIBUTIONS.md).
