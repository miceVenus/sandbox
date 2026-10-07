set(SANDBOX_BUILD_HELPER_DIR "${CMAKE_CURRENT_BINARY_DIR}/libexec/bbm-sandbox")
if(SANDBOX_DEVELOPMENT_BUILD)
  set(SANDBOX_RESOURCE_DIR "${SANDBOX_BUILD_HELPER_DIR}")
else()
  set(SANDBOX_RESOURCE_DIR "${CMAKE_INSTALL_FULL_LIBEXECDIR}/bbm-sandbox")
endif()

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


sandbox_add_helper(sandbox-crun cxx_std_17
        src/virtualization/container/crun_worker.cpp
        src/virtualization/container/crun_ops.c)

target_compile_features(sandbox-crun PRIVATE c_std_11)
target_link_libraries(sandbox-crun PRIVATE Libcrun::Libcrun nlohmann_json::nlohmann_json)

sandbox_add_helper(agentd cxx_std_17
  src/agentd/main.cpp src/agentd/task_runner.cpp)
# Service sources and shared implementation are attached in SandboxLibrary.cmake.

if(SANDBOX_ENABLE_LIBKRUN)
  sandbox_add_helper(sandbox-krun cxx_std_17 src/virtualization/microvm/krun_runner.cpp)
  target_link_libraries(sandbox-krun PRIVATE Libkrun::Libkrun nlohmann_json::nlohmann_json)
  # Stage private copies; never patch libraries in an external installation.
  find_program(SANDBOX_PATCHELF patchelf REQUIRED)
  file(MAKE_DIRECTORY "${SANDBOX_BUILD_HELPER_DIR}/lib")
  foreach(pair "${LIBKRUN_LIBRARY}|libkrun.so.1" "${LIBKRUNFW_LIBRARY}|libkrunfw.so.5")
    string(REPLACE "|" ";" parts "${pair}")
    list(GET parts 0 source)
    list(GET parts 1 name)
    execute_process(COMMAND "${SANDBOX_PATCHELF}" --print-soname "${source}"
      OUTPUT_VARIABLE soname OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE inspected)
    if(NOT inspected EQUAL 0 OR NOT soname STREQUAL name)
      message(FATAL_ERROR "${source} has an unsupported ABI; expected SONAME ${name}, found ${soname}")
    endif()
    set(destination "${SANDBOX_BUILD_HELPER_DIR}/lib/${name}")
    file(COPY_FILE "${source}" "${destination}" ONLY_IF_DIFFERENT)
    execute_process(COMMAND "${SANDBOX_PATCHELF}" --set-rpath "$ORIGIN" "${destination}"
      RESULT_VARIABLE patched)
    if(NOT patched EQUAL 0)
      message(FATAL_ERROR "Cannot prepare private runtime library ${destination}")
    endif()
  endforeach()
  # The executable and both dependent DSOs use paths relative to their own location.
  set_target_properties(sandbox-krun PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "$ORIGIN/lib")
  install(DIRECTORY "${SANDBOX_BUILD_HELPER_DIR}/lib/"
    DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/bbm-sandbox/lib")
endif()
