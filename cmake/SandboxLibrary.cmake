# Shared implementation is compiled once and embedded in both the SDK and agentd.
# agentd links only its service code and these shared objects, never the host SDK.
add_library(sandbox_shared OBJECT
  src/lib/error.cpp
  src/lib/string.cpp
  src/lib/filesystem.cpp
  src/lib/descriptor.cpp
  src/lib/json.cpp
  src/lib/memory_file.cpp
  src/lib/elf.cpp
  src/lib/process.cpp
  src/ipc/io.cpp
  src/ipc/socket.cpp
  src/ipc/protocol.cpp
  src/ipc/session.cpp)
target_compile_features(sandbox_shared PRIVATE cxx_std_17)
target_compile_options(sandbox_shared PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(sandbox_shared PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(sandbox_shared PRIVATE nlohmann_json::nlohmann_json Threads::Threads)
set_target_properties(sandbox_shared PROPERTIES POSITION_INDEPENDENT_CODE ON FOLDER "SDK/Internal")

add_library(sandbox_core
  src/sandbox.cpp
  src/workspace/workspace.cpp
  src/workspace/git_ops.cpp
  src/virtualization/container/crun_runtime.cpp
  src/virtualization/microvm/krun_runtime.cpp
  src/virtualization/container_client.cpp
  src/virtualization/agentd_client.cpp
  src/virtualization/environment/rootfs.cpp
  src/virtualization/environment/host_tools.cpp
  src/virtualization/environment/oci.cpp
  src/virtualization/container/resources.cpp
  src/resources.cpp
  $<TARGET_OBJECTS:sandbox_shared>)
add_library(bbm::sandbox_core ALIAS sandbox_core)
target_compile_features(sandbox_core PUBLIC cxx_std_17)
target_compile_options(sandbox_core PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(sandbox_core
  PUBLIC nlohmann_json::nlohmann_json Threads::Threads
  PRIVATE libgit2::libgit2package)
target_include_directories(sandbox_core
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/bbm-sandbox>
  PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
set_target_properties(sandbox_core PROPERTIES FOLDER "SDK")
add_dependencies(sandbox_core sandbox-crun)
if(SANDBOX_ENABLE_LIBKRUN)
  target_compile_definitions(sandbox_core PRIVATE SANDBOX_HAS_LIBKRUN=1)
endif()

target_sources(agentd PRIVATE
  src/agentd/service.cpp
  src/agentd/workspace_files.cpp
  src/agentd/container_bootstrap.cpp
  src/agentd/microvm_bootstrap.cpp
  $<TARGET_OBJECTS:sandbox_shared>)
target_link_libraries(agentd PRIVATE nlohmann_json::nlohmann_json Threads::Threads)
