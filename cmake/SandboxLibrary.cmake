add_library(sandbox_core
        src/sandbox.cpp
        src/virtualization/process.cpp
        src/workspace/workspace.cpp
        src/workspace/workspace_backend.cpp
        src/workspace/workspace_files.cpp
        src/workspace/git/git_transport.cpp
        src/virtualization/container/oci_runtime.cpp
        src/virtualization/oci.cpp
        src/virtualization/bundle.cpp
        src/virtualization/container/crun_worker_client.cpp
        src/virtualization/microvm/libkrun_runtime.cpp
        src/virtualization/runtime_files.cpp
        src/virtualization/runtime_policy.cpp
        src/resources.cpp
        src/virtualization/host_tools.cpp
        src/guest/guest_service.cpp
        src/guest/guest_vm.cpp
        src/virtualization/microvm/communication/agent_protocol.cpp
        src/virtualization/microvm/communication/agent_transport.cpp
        src/agent_client.cpp
        src/lib.cpp)

target_include_directories(sandbox_core
        PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/bbm-sandbox>
)

add_library(bbm::sandbox_core ALIAS sandbox_core)

target_compile_features(sandbox_core PUBLIC cxx_std_17)
target_compile_options(sandbox_core PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(sandbox_core PUBLIC nlohmann_json::nlohmann_json Threads::Threads)
target_include_directories(sandbox_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/bbm-sandbox>
  PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
set_target_properties(sandbox_core PROPERTIES FOLDER "SDK")

add_dependencies(sandbox_core sandbox-io sandbox-crun sandbox-git)
target_link_libraries(agentd PRIVATE sandbox_core)
if(SANDBOX_ENABLE_LIBKRUN)
  target_compile_definitions(sandbox_core PRIVATE SANDBOX_HAS_LIBKRUN=1)
endif()
