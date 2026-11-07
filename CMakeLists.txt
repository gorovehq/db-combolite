cmake_minimum_required(VERSION 3.16)

project(combolite_umbrella VERSION 0.1.0 LANGUAGES C)

enable_testing()

# Unified test framework interface
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/combolite-tests/CMakeLists.txt)
  add_subdirectory(combolite-tests EXCLUDE_FROM_ALL)
endif()

# Core engine (and accelerators, tests, and demo if enabled)
add_subdirectory(combolite-core)

# Combolite Studio tests
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/combolite-studio/tests)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    add_test(NAME "combolite-studio:python_tests"
      COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/combolite-studio/tests
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/combolite-studio)
    set_tests_properties("combolite-studio:python_tests" PROPERTIES LABELS "combolite-studio")
  endif()
endif()
