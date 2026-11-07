cmake_minimum_required(VERSION 3.16)

project(combolite_umbrella VERSION 0.1.0 LANGUAGES C)

enable_testing()

# Unified test framework interface
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/combolite-tests/CMakeLists.txt)
  add_subdirectory(combolite-tests EXCLUDE_FROM_ALL)
endif()

# Core engine (and accelerators, tests, and demo if enabled)
add_subdirectory(combolite-core)
