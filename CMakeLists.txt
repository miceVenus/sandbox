cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../vcpkg/scripts/buildsystems/vcpkg.cmake")
  set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "vcpkg toolchain")
endif()
project(crun_sandbox_starter LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
option(SANDBOX_DEVELOPMENT_BUILD "Use SDK resources from this build tree" ON)

set(SANDBOX_BUILD_HELPER_DIR "${CMAKE_CURRENT_BINARY_DIR}/libexec/bbm-sandbox")
if(SANDBOX_DEVELOPMENT_BUILD)
  set(SANDBOX_FILE_HELPER_PATH "${SANDBOX_BUILD_HELPER_DIR}/sandbox-io")
else()
  set(SANDBOX_FILE_HELPER_PATH "${CMAKE_INSTALL_FULL_LIBEXECDIR}/bbm-sandbox/sandbox-io")
endif()
configure_file(resources_config.hpp.in generated/resources_config.hpp @ONLY)

add_executable(sandbox-io file_io_helper.cpp)
target_compile_features(sandbox-io PRIVATE cxx_std_17)
target_compile_options(sandbox-io PRIVATE -Wall -Wextra -Wpedantic)
# A generator expression keeps multi-config generators from adding a subdirectory.
set_target_properties(sandbox-io PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "$<1:${SANDBOX_BUILD_HELPER_DIR}>")

add_library(sandbox_core process.cpp workspace.cpp
  client.hpp
  client.cpp
  sandbox.hpp
  sandbox.cpp
  oci.hpp
  oci.cpp
  bundle.hpp
  bundle.cpp
  resources.hpp
  resources.cpp
  host_tools.hpp
  host_tools.cpp
  lib.cpp
  lib.hpp)
add_dependencies(sandbox_core sandbox-io)

find_package(nlohmann_json CONFIG REQUIRED)

target_link_libraries(sandbox_core PUBLIC nlohmann_json::nlohmann_json)
target_include_directories(sandbox_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/bbm-sandbox>
  PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_library(bbm::sandbox_core ALIAS sandbox_core)
add_executable(sandboxctl main.cpp)
target_link_libraries(sandboxctl PRIVATE sandbox_core)
set_target_properties(sandboxctl PROPERTIES INSTALL_RPATH "${CMAKE_INSTALL_FULL_LIBDIR}")

target_compile_features(sandbox_core PUBLIC cxx_std_17)
target_compile_options(sandbox_core PRIVATE -Wall -Wextra -Wpedantic)
include(CTest)

# A development library contains a build-tree resource path. Do not distribute it.
if(SANDBOX_DEVELOPMENT_BUILD)
  install(CODE "message(FATAL_ERROR \"Configure an installation build with -DSANDBOX_DEVELOPMENT_BUILD=OFF and set CMAKE_INSTALL_PREFIX before building\")")
endif()
install(TARGETS sandbox-io
  RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/bbm-sandbox)
install(TARGETS sandboxctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS sandbox_core EXPORT bbm-sandbox-targets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES sandbox.hpp client.hpp workspace.hpp process.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/bbm-sandbox)
install(EXPORT bbm-sandbox-targets NAMESPACE bbm::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/bbm-sandbox)
configure_package_config_file(cmake/bbm-sandbox-config.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/bbm-sandbox-config.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/bbm-sandbox)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/bbm-sandbox-config.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/bbm-sandbox)
if(BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME process-supervisor COMMAND sandboxctl self-test)
  add_test(NAME git-workspace COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_workspace.py $<TARGET_FILE:sandboxctl>)
endif()

if(BUILD_TESTING)
  add_executable(manager-test tests/manager_test.cpp)
  target_link_libraries(manager-test PRIVATE sandbox_core)
  add_test(NAME session-manager COMMAND manager-test)
  add_test(NAME manager-crun COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_manager_crun.py $<TARGET_FILE:sandboxctl>)
  set_tests_properties(manager-crun PROPERTIES TIMEOUT 60)
  add_test(NAME manager-crun-minimal COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_manager_crun.py $<TARGET_FILE:sandboxctl> minimal)
  set_tests_properties(manager-crun-minimal PROPERTIES TIMEOUT 60)
  add_test(NAME sdk-install COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sdk_install.py
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_COMMAND} ${nlohmann_json_DIR})
  set_tests_properties(sdk-install PROPERTIES TIMEOUT 120)
endif()
