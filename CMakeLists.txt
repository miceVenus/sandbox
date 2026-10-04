cmake_minimum_required(VERSION 3.21)

# A caller-supplied toolchain takes precedence over the adjacent vcpkg checkout.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../vcpkg/scripts/buildsystems/vcpkg.cmake")
  set(CMAKE_TOOLCHAIN_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/../vcpkg/scripts/buildsystems/vcpkg.cmake"
    CACHE FILEPATH "vcpkg toolchain")
endif()

project(bbm_sandbox LANGUAGES C CXX)

option(BUILD_TESTING "Build the SDK test suite" OFF)
option(SANDBOX_DEVELOPMENT_BUILD "Use SDK resources from this build tree" ON)
option(SANDBOX_ENABLE_LIBKRUN "Build the libkrun VM backend" OFF)
option(SANDBOX_BUILD_BENCHMARKS "Build opt-in SDK performance measurements" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)
include(SandboxDependencies)
include(SandboxHelpers)
include(SandboxLibrary)
include(SandboxInstall)

# One SDK entry point builds the library and every required runtime resource.
add_custom_target(sandbox-sdk ALL
  DEPENDS sandbox_core ${SANDBOX_HELPER_TARGETS})
set_target_properties(sandbox-sdk PROPERTIES FOLDER "SDK")

if(BUILD_TESTING)
  enable_testing()
  include(SandboxTests)
endif()

if(SANDBOX_BUILD_BENCHMARKS)
  include(SandboxBenchmarks)
endif()
