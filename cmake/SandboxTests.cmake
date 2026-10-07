find_package(Python3 COMPONENTS Interpreter REQUIRED)

function(sandbox_add_test_program name)
  add_executable(${name}-test EXCLUDE_FROM_ALL tests/${name}_test.cpp)
  target_link_libraries(${name}-test PRIVATE sandbox_core)
  add_dependencies(${name}-test sandbox-sdk)
  set_target_properties(${name}-test PROPERTIES FOLDER "Tests")
  set(SANDBOX_TEST_TARGETS ${SANDBOX_TEST_TARGETS} ${name}-test PARENT_SCOPE)
endfunction()

foreach(name IN ITEMS process workspace sandbox backend container agentd_protocol agentd_service session)
  sandbox_add_test_program(${name})
endforeach()
target_sources(session-test PRIVATE src/agentd/service.cpp)
add_test(NAME agentd-session COMMAND session-test)
set_tests_properties(agentd-session PROPERTIES TIMEOUT 20)

add_test(NAME process-supervisor COMMAND process-test)
add_test(NAME git-workspace COMMAND workspace-test)
add_test(NAME sandbox-lifecycle COMMAND sandbox-test)
add_test(NAME runtime-backend COMMAND backend-test)
add_test(NAME agentd-protocol COMMAND agentd_protocol-test)
add_test(NAME agentd-service COMMAND agentd_service-test $<TARGET_FILE:agentd>)
set_tests_properties(agentd-protocol agentd-service PROPERTIES TIMEOUT 30)
add_test(NAME container-crun COMMAND container-test host-tools)
add_test(NAME container-crun-minimal COMMAND container-test minimal)
set_tests_properties(container-crun container-crun-minimal PROPERTIES TIMEOUT 60)

add_test(NAME sdk-install COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sdk_install.py
  ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_COMMAND} ${nlohmann_json_DIR} ${libgit2_DIR}
  ${SANDBOX_ENABLE_LIBKRUN} "${LIBKRUN_ROOT}" "${LIBCRUN_ROOT}" "${SANDBOX_DEPS_PREFIX}")
set_tests_properties(sdk-install PROPERTIES TIMEOUT 240)

if(SANDBOX_ENABLE_LIBKRUN)
  sandbox_add_test_program(microvm)
  add_test(NAME microvm-krun-minimal COMMAND microvm-test minimal)
  add_test(NAME microvm-krun-host-tools COMMAND microvm-test host-tools)
  set_tests_properties(microvm-krun-minimal microvm-krun-host-tools
    PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
endif()

add_custom_target(sandbox-tests ALL DEPENDS sandbox-sdk ${SANDBOX_TEST_TARGETS})
set_target_properties(sandbox-tests PROPERTIES FOLDER "Tests")
