set(SANDBOX_BUILD_HELPER_DIR "${CMAKE_CURRENT_BINARY_DIR}/libexec/bbm-sandbox")
if(SANDBOX_DEVELOPMENT_BUILD)
  set(SANDBOX_RESOURCE_DIR "${SANDBOX_BUILD_HELPER_DIR}")
else()
  set(SANDBOX_RESOURCE_DIR "${CMAKE_INSTALL_FULL_LIBEXECDIR}/bbm-sandbox")
endif()

set(SANDBOX_FILE_HELPER_PATH "${SANDBOX_RESOURCE_DIR}/sandbox-io")
set(SANDBOX_RUNTIME_RUNNER_PATH "${SANDBOX_RESOURCE_DIR}/sandbox-crun")
set(SANDBOX_GIT_WORKER_PATH "${SANDBOX_RESOURCE_DIR}/sandbox-git")
set(SANDBOX_GUEST_AGENT_PATH "${SANDBOX_RESOURCE_DIR}/agentd")
set(SANDBOX_KRUN_RUNNER_PATH "${SANDBOX_RESOURCE_DIR}/sandbox-krun")
configure_file(resources_config.hpp.in generated/resources_config.hpp @ONLY)

function(sandbox_add_helper target standard)
  add_executable(${target} ${ARGN})
  target_include_directories(${target} PRIVATE
          "${PROJECT_SOURCE_DIR}/include")
  target_compile_features(${target} PRIVATE ${standard})
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  # Keep multi-config generators from adding another output subdirectory.
  set_target_properties(${target} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "$<1:${SANDBOX_BUILD_HELPER_DIR}>"
    FOLDER "SDK/Internal")
  set(SANDBOX_HELPER_TARGETS ${SANDBOX_HELPER_TARGETS} ${target} PARENT_SCOPE)
endfunction()


sandbox_add_helper(sandbox-io cxx_std_17
        src/virtualization/container/file_io_helper.cpp
        src/workspace/workspace_files.cpp)

sandbox_add_helper(sandbox-crun c_std_11
        src/virtualization/container/crun_runner.c)

target_link_libraries(sandbox-crun PRIVATE Libcrun::Libcrun)

sandbox_add_helper(sandbox-git cxx_std_17
        src/workspace/git/git_runner.cpp
        src/workspace/git/git_repository_ops.cpp
        src/lib.cpp)

target_link_libraries(sandbox-git PRIVATE
  nlohmann_json::nlohmann_json libgit2::libgit2package)

sandbox_add_helper(agentd cxx_std_17
  src/guest/guest_main.cpp src/guest/task_runner.cpp)
# sandbox_core is declared in SandboxLibrary.cmake; link there to avoid a cycle.

if(SANDBOX_ENABLE_LIBKRUN)
  sandbox_add_helper(sandbox-krun cxx_std_17 src/virtualization/microvm/krun_runner.cpp)
  target_link_libraries(sandbox-krun PRIVATE Libkrun::Libkrun nlohmann_json::nlohmann_json)
  get_filename_component(SANDBOX_KRUN_LIBRARY_DIR "${LIBKRUN_LIBRARY}" DIRECTORY)
  set_target_properties(sandbox-krun PROPERTIES
    INSTALL_RPATH "${SANDBOX_KRUN_LIBRARY_DIR}")
endif()