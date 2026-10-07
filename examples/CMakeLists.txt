cmake_minimum_required(VERSION 3.21)

# Also usable as a separate application against an installed SDK.
if(NOT TARGET bbm::sandbox_core)
  project(sandbox_example LANGUAGES CXX)
  find_package(bbm-sandbox CONFIG REQUIRED)
endif()

add_executable(sandbox-example sdk_example.cpp)
target_link_libraries(sandbox-example PRIVATE bbm::sandbox_core)
target_compile_options(sandbox-example PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(sandbox-example PROPERTIES FOLDER "Examples")
if(TARGET sandbox-sdk)
  add_dependencies(sandbox-example sandbox-sdk)
endif()
