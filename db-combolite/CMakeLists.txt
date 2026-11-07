cmake_minimum_required(VERSION 3.16)

project(combolite VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(CDB_BUILD_TESTS "Build the test suite" ON)
option(CDB_WERROR "Treat warnings as errors" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Sanitizers are on by default in Debug builds.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(CDB_SANITIZE_DEFAULT ON)
else()
  set(CDB_SANITIZE_DEFAULT OFF)
endif()
option(CDB_SANITIZE "Build with AddressSanitizer and UBSan" ${CDB_SANITIZE_DEFAULT})

# Build profile (spec 17): host sizes a process's limits for a host and
# grows working memory through the environment's allocator; mcu keeps
# everything in caller memory. Both read and write the same files.
set(CDB_PROFILE host CACHE STRING "Build profile: host or mcu (spec 17)")
set_property(CACHE CDB_PROFILE PROPERTY STRINGS host mcu)
if(NOT CDB_PROFILE STREQUAL "host" AND NOT CDB_PROFILE STREQUAL "mcu")
  message(FATAL_ERROR "CDB_PROFILE must be host or mcu, not '${CDB_PROFILE}'")
endif()
# Host optimisation (17.6): -O3 and link-time optimisation in optimised
# builds; -march=native only on request, for builds that never leave this
# machine.
option(CDB_NATIVE "Optimise for this machine's CPU (-march=native); never for distributed builds" OFF)

# Third-party sources are git submodules (third-party/*.VENDORED.md). A
# clone without --recursive, or a pull that added them, leaves their folders
# empty: say what to run instead of failing later on a missing file.
foreach(sub third-party/mbedtls third-party/mbedtls/framework third-party/xz third-party/unity)
  if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${sub}/CMakeLists.txt
     AND NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${sub}/src/unity.c)
    message(FATAL_ERROR "${sub} is empty: run `git submodule update --init --recursive` in the repository root.")
  endif()
endforeach()

# LZMA2 compression through the vendored liblzma (spec 15.7,
# third-party/xz.VENDORED.md). Without it, compression defaults are off and
# files that use compression are refused.
option(CDB_WITH_XZ "Build the xz (LZMA2) codec" ON)
if(CDB_WITH_XZ)
  # Only what spec 15 uses; forced, so xz's own defaults never apply.
  set(XZ_NLS OFF CACHE BOOL "" FORCE)
  set(XZ_THREADS no CACHE STRING "" FORCE)
  set(XZ_DOC OFF CACHE BOOL "" FORCE)
  set(XZ_SANDBOX no CACHE STRING "" FORCE)
  set(XZ_ENCODERS "lzma1;lzma2" CACHE STRING "" FORCE)
  set(XZ_DECODERS "lzma1;lzma2" CACHE STRING "" FORCE)
  set(XZ_MATCH_FINDERS "hc3;hc4;bt4" CACHE STRING "" FORCE)
  set(XZ_CHECKS "crc32" CACHE STRING "" FORCE)
  foreach(off XZ_MICROLZMA_ENCODER XZ_MICROLZMA_DECODER XZ_LZIP_DECODER XZ_TOOL_XZ XZ_TOOL_XZDEC
              XZ_TOOL_LZMADEC XZ_TOOL_LZMAINFO XZ_TOOL_SCRIPTS XZ_TOOL_SYMLINKS XZ_TOOL_SYMLINKS_LZMA)
    set(${off} OFF CACHE BOOL "" FORCE)
  endforeach()
  # The submodule carries xz's own tests, whose targets (test_index, ...)
  # clash with ours: keep them out.
  set(CDB_SAVED_BUILD_TESTING ${BUILD_TESTING})
  set(BUILD_TESTING OFF)
  add_subdirectory(third-party/xz EXCLUDE_FROM_ALL)
  set(BUILD_TESTING ${CDB_SAVED_BUILD_TESTING})
endif()

# Mbed TLS crypto library (spec 12, third-party/mbedtls.VENDORED.md).
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(INSTALL_MBEDTLS_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(third-party/mbedtls EXCLUDE_FROM_ALL)

find_package(Threads REQUIRED)

set(CDB_WARNINGS
  -Wall -Wextra -Wpedantic
  -Wshadow -Wconversion -Wsign-conversion
  -Wstrict-prototypes -Wmissing-prototypes
  -Wcast-align -Wvla -Wundef)
if(CDB_WERROR)
  list(APPEND CDB_WARNINGS -Werror)
endif()

set(CDB_SANITIZER_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)

add_library(combolite
  src/cdb_status.c
  src/cdb_crc32c.c
  src/cdb_varint.c
  src/cdb_value.c
  src/cdb_hostfile.c
  src/cdb_header.c
  src/cdb_page.c
  src/cdb_pager.c
  src/cdb_overflow.c
  src/cdb_blob.c
  src/cdb_hostenv.c
  src/cdb_journal.c
  src/cdb_coord.c
  src/cdb_db.c
  src/cdb_slots.c
  src/cdb_nib.c
  src/cdb_codec.c
  src/cdb_colenc.c
  src/cdb_rowgroup.c
  src/cdb_table.c
  src/cdb_key.c
  src/cdb_index.c
  src/cdb_catroot.c
  src/cdb_catalog.c
  src/cdb_doc.c
  src/cdb_schema.c
  src/cdb_fts.c
  src/cdb_vec.c
  src/cdb_link.c
  src/cdb_ts.c
  src/cdb_tsbits.c
  src/cdb_tsblock.c
  src/cdb_worker.c
  src/cdb_query.c
  src/cdb_eval.c
  src/cdb_math.c
  src/cdb_exec.c
  src/cdb_proc.c
  src/cdb_lexer.c
  src/cdb_compiler.c
  src/cdb_trigger.c
  src/cdb_crypto.c
  src/cdb_audit.c
  src/cdb_crdt.c)
target_include_directories(combolite
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
  SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third-party/mbedtls/include)
target_compile_options(combolite PRIVATE ${CDB_WARNINGS})
target_link_libraries(combolite PUBLIC Threads::Threads PRIVATE mbedcrypto)
if(UNIX)
  target_link_libraries(combolite PUBLIC m) # log() for BM25 ranking
endif()
if(CDB_WITH_XZ)
  target_link_libraries(combolite PRIVATE liblzma)
  target_include_directories(combolite SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third-party/xz/src/liblzma/api)
  target_compile_definitions(combolite PUBLIC CDB_WITH_XZ=1)
endif()
if(CDB_SANITIZE)
  target_compile_options(combolite PUBLIC ${CDB_SANITIZER_FLAGS})
  target_link_options(combolite PUBLIC ${CDB_SANITIZER_FLAGS})
endif()
if(CDB_PROFILE STREQUAL "mcu")
  target_compile_definitions(combolite PUBLIC CDB_PROFILE_MCU=1)
  target_compile_options(combolite PRIVATE $<$<NOT:$<CONFIG:Debug>>:-Os>)
else()
  target_compile_options(combolite PRIVATE $<$<NOT:$<CONFIG:Debug>>:-O3>)
  if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT CDB_IPO_OK OUTPUT CDB_IPO_MSG LANGUAGES C)
    if(CDB_IPO_OK)
      set_property(TARGET combolite PROPERTY INTERPROCEDURAL_OPTIMIZATION ON)
    endif()
  endif()
  if(CDB_NATIVE)
    target_compile_options(combolite PRIVATE -march=native)
  endif()
endif()
message(STATUS "CDB_PROFILE=${CDB_PROFILE}")

# The optional accelerator library, combolite_accel (spec 18.1, 18.5):
# never part of the core, host profile only, with a CUDA backend, a Vulkan
# one, or both. CUDA is enabled here, at the top, because the tests link
# its kernels too.
option(CDB_ACCEL_CUDA "Build the CUDA accelerator (needs the CUDA toolkit with cuBLAS)" OFF)
option(CDB_ACCEL_VULKAN "Build the Vulkan accelerator (needs the Vulkan loader and headers)" OFF)
if((CDB_ACCEL_CUDA OR CDB_ACCEL_VULKAN) AND NOT CDB_PROFILE STREQUAL "host")
  message(FATAL_ERROR "The accelerator exists only in the host profile (spec 18.1)")
endif()
if(CDB_ACCEL_CUDA)
  if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
    set(CMAKE_CUDA_ARCHITECTURES native)
  endif()
  # NVIDIA's packages install outside PATH; CUDACXX or CMAKE_CUDA_COMPILER
  # still choose another.
  if(NOT DEFINED CMAKE_CUDA_COMPILER AND NOT DEFINED ENV{CUDACXX} AND EXISTS /usr/local/cuda/bin/nvcc)
    set(CMAKE_CUDA_COMPILER /usr/local/cuda/bin/nvcc)
  endif()
  enable_language(CUDA)
  find_package(CUDAToolkit REQUIRED)
endif()
if(CDB_ACCEL_VULKAN)
  find_package(Vulkan REQUIRED)
endif()
if(CDB_ACCEL_CUDA OR CDB_ACCEL_VULKAN)
  add_subdirectory(accel)
endif()

if(CDB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

option(CDB_BUILD_DEMO "Build the GutenFree Diet demo (needs ncurses)" OFF)
if(CDB_BUILD_DEMO)
  add_subdirectory(demo/gutenfree)
endif()
