# GutenFree Diet, the demo e-reader. Built only with -DCDB_BUILD_DEMO=ON, so
# the engine never needs ncurses or llama.cpp. The demo includes src/ the
# way the tests do: the model APIs are internal headers for now.
set(CURSES_NEED_WIDE TRUE)
find_package(Curses REQUIRED)

# llama.cpp, vendored (third-party/llama.cpp/VENDORED.md), for embeddings
# only: the library, without its tools, tests, examples or network code.
enable_language(CXX)
set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_APP OFF CACHE BOOL "" FORCE)
set(LLAMA_OPENSSL OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_HTML OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(GGML_NATIVE ON CACHE BOOL "" FORCE)
# The vendored tree has no .git: name the pinned release, not this repo's commit.
set(LLAMA_BUILD_NUMBER 11701)
set(LLAMA_BUILD_COMMIT 01fc3aa)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../third-party/llama.cpp
                 ${CMAKE_CURRENT_BINARY_DIR}/llama.cpp EXCLUDE_FROM_ALL)

add_executable(gutenfree gf_main.c gf_db.c gf_catalog.c gf_books.c gf_metrics.c gf_embed.c gf_vec.c gf_probe.c gf_jobs.c gf_ui.c)
target_include_directories(gutenfree PRIVATE
  ${PROJECT_SOURCE_DIR}/src
  ${CURSES_INCLUDE_DIRS})
target_compile_options(gutenfree PRIVATE ${CDB_WARNINGS})
# llama.h and ggml.h use C11 and GNU forms (_Noreturn, repeated typedefs,
# anonymous structs); only gf_embed.c includes them.
set_source_files_properties(gf_embed.c PROPERTIES COMPILE_OPTIONS "-Wno-pedantic")
# Shown on screen, so timings are never read without knowing the build.
if(CDB_SANITIZE)
  set(GF_BUILD_LABEL "${CMAKE_BUILD_TYPE} + ASan/UBSan: timings about 9x slow")
else()
  set(GF_BUILD_LABEL "${CMAKE_BUILD_TYPE}")
endif()
target_compile_definitions(gutenfree PRIVATE GF_BUILD_LABEL="${GF_BUILD_LABEL}")
target_link_libraries(gutenfree PRIVATE combolite llama ${CURSES_LIBRARIES})
# The accelerator (spec 18), when this build has a backend: vector search
# and graph refreshes on the GPU (gf_db.c; GUTENFREE_ACCEL turns it off).
if(TARGET combolite_accel)
  target_link_libraries(gutenfree PRIVATE combolite_accel)
  target_compile_definitions(gutenfree PRIVATE GF_ACCEL=1)
endif()
# The binary goes next to its scripts and data/, so it runs as ./gutenfree
# from demo/gutenfree/ (the last build made wins; git ignores it).
set_target_properties(gutenfree PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
