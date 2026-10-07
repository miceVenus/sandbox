include(CMakePackageConfigHelpers)

# Development libraries contain build-tree paths; use the release preset to install.
if(SANDBOX_DEVELOPMENT_BUILD)
  install(CODE "message(FATAL_ERROR \"Configure an installation build with -DSANDBOX_DEVELOPMENT_BUILD=OFF and set CMAKE_INSTALL_PREFIX before building\")")
endif()

install(TARGETS ${SANDBOX_HELPER_TARGETS}
  RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/bbm-sandbox)
install(TARGETS sandbox_core EXPORT bbm-sandbox-targets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# Preserve the public include tree after the source layout refactor. Backend
# workers, bootstrap policies and their dependency headers remain private.
install(FILES include/sandbox.hpp include/sandbox_types.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/bbm-sandbox)
install(FILES include/virtualization/runtime.hpp include/virtualization/agentd_client.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/bbm-sandbox/virtualization)
install(FILES include/virtualization/microvm/krun_runtime.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/bbm-sandbox/virtualization/microvm)
install(FILES include/workspace/workspace.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/bbm-sandbox/workspace)
install(FILES include/ipc/transport.hpp include/ipc/protocol.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/bbm-sandbox/ipc)
install(DIRECTORY include/lib/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/bbm-sandbox/lib
  FILES_MATCHING PATTERN "*.hpp")
install(EXPORT bbm-sandbox-targets NAMESPACE bbm::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/bbm-sandbox)
configure_package_config_file(cmake/bbm-sandbox-config.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/bbm-sandbox-config.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/bbm-sandbox)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/bbm-sandbox-config.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/bbm-sandbox)
