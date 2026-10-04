if(NOT SANDBOX_ENABLE_LIBKRUN)
  message(FATAL_ERROR "SDK benchmarks require SANDBOX_ENABLE_LIBKRUN=ON")
endif()

add_executable(sandbox-benchmark EXCLUDE_FROM_ALL benchmarks/sdk_benchmark.cpp)
target_link_libraries(sandbox-benchmark PRIVATE sandbox_core)
target_compile_options(sandbox-benchmark PRIVATE -Wall -Wextra -Wpedantic)
add_dependencies(sandbox-benchmark sandbox-sdk)
set_target_properties(sandbox-benchmark PROPERTIES FOLDER "Benchmarks")
