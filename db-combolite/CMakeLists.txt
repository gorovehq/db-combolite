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

# LZMA2 compression through the vendored liblzma (spec 15.7,
# third-party/xz/VENDORED.md). Without it, compression defaults are off and
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
  add_subdirectory(third-party/xz EXCLUDE_FROM_ALL)
endif()

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
  src/cdb_query.c)
target_include_directories(combolite
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(combolite PRIVATE ${CDB_WARNINGS})
target_link_libraries(combolite PUBLIC Threads::Threads)
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

if(CDB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

option(CDB_BUILD_DEMO "Build the GutenFree Diet demo (needs ncurses)" OFF)
if(CDB_BUILD_DEMO)
  add_subdirectory(demo/gutenfree)
endif()
