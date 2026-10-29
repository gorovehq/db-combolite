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
  src/cdb_header.c)
target_include_directories(combolite
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(combolite PRIVATE ${CDB_WARNINGS})
if(CDB_SANITIZE)
  target_compile_options(combolite PUBLIC ${CDB_SANITIZER_FLAGS})
  target_link_options(combolite PUBLIC ${CDB_SANITIZER_FLAGS})
endif()

if(CDB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
