find_package(Python3 COMPONENTS Interpreter REQUIRED)

function(sandbox_add_test_program name)
  add_executable(${name}-test EXCLUDE_FROM_ALL tests/${name}_test.cpp)
  target_link_libraries(${name}-test PRIVATE sandbox_core)
  add_dependencies(${name}-test sandbox-sdk)
  set_target_properties(${name}-test PROPERTIES FOLDER "Tests")
  set(SANDBOX_TEST_TARGETS ${SANDBOX_TEST_TARGETS} ${name}-test PARENT_SCOPE)
endfunction()

foreach(name IN ITEMS process workspace manager backend manager_crun agent_protocol guest_service)
  sandbox_add_test_program(${name})
endforeach()
add_test(NAME process-supervisor COMMAND process-test)
add_test(NAME git-workspace COMMAND workspace-test)
add_test(NAME sandbox-lifecycle COMMAND manager-test)
add_test(NAME runtime-backend COMMAND backend-test)
add_test(NAME agent-protocol COMMAND agent_protocol-test)
add_test(NAME guest-service COMMAND guest_service-test $<TARGET_FILE:agentd>)
set_tests_properties(agent-protocol guest-service PROPERTIES TIMEOUT 30)
add_test(NAME manager-crun COMMAND manager_crun-test host-tools)
add_test(NAME manager-crun-minimal COMMAND manager_crun-test minimal)
set_tests_properties(manager-crun manager-crun-minimal PROPERTIES TIMEOUT 60)

add_test(NAME sdk-install COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sdk_install.py
  ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_COMMAND} ${nlohmann_json_DIR} ${libgit2_DIR}
  ${SANDBOX_ENABLE_LIBKRUN} "${LIBKRUN_ROOT}")
set_tests_properties(sdk-install PROPERTIES TIMEOUT 120)

if(SANDBOX_ENABLE_LIBKRUN)
  sandbox_add_test_program(manager_krun)
  add_test(NAME manager-krun-minimal COMMAND manager_krun-test minimal)
  add_test(NAME manager-krun-host-tools COMMAND manager_krun-test host-tools)
  set_tests_properties(manager-krun-minimal manager-krun-host-tools
    PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
endif()

add_custom_target(sandbox-tests ALL DEPENDS sandbox-sdk ${SANDBOX_TEST_TARGETS})
set_target_properties(sandbox-tests PROPERTIES FOLDER "Tests")
