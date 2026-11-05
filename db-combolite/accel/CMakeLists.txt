# combolite_accel (spec 18): the optional accelerator library. An
# application links it and installs its hook in cdb_env; the core never
# calls a GPU API.

# The kernels: CUDA C++, kept apart so the core's C flags and sanitizers
# never reach nvcc. The shared CUDA runtime, as the host code below links.
add_library(combolite_accel_kernels STATIC cuda/cak_kernels.cu)
set_target_properties(combolite_accel_kernels PROPERTIES
  CUDA_STANDARD 17
  CUDA_RUNTIME_LIBRARY Shared
  POSITION_INDEPENDENT_CODE ON)

# The host code: C99 under the project's warnings, on the CUDA runtime's
# and cuBLAS's C interfaces.
add_library(combolite_accel STATIC cuda/cdb_accel_cuda.c)
target_include_directories(combolite_accel
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cuda)
target_compile_options(combolite_accel PRIVATE ${CDB_WARNINGS})
target_link_libraries(combolite_accel
  PUBLIC combolite
  PRIVATE combolite_accel_kernels CUDA::cudart CUDA::cublas)
target_compile_definitions(combolite_accel PUBLIC CDB_ACCEL_CUDA=1)
