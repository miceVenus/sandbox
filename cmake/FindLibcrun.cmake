# libcrun installations vary. A configured source tree supplies generated OCI
# headers when the local static installation did not install development headers.
set(SANDBOX_LIBCRUN_SOURCE_DIR "" CACHE PATH "Configured crun source/build tree matching libcrun")
if(NOT SANDBOX_LIBCRUN_SOURCE_DIR AND EXISTS "${PROJECT_SOURCE_DIR}/../../build/crun/config.h")
  get_filename_component(SANDBOX_LIBCRUN_SOURCE_DIR "${PROJECT_SOURCE_DIR}/../../build/crun" ABSOLUTE)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(CRUN_SUPPORT REQUIRED IMPORTED_TARGET libsystemd libseccomp libcap json-c)
find_path(LIBCRUN_INCLUDE_DIR libcrun/container.h
  HINTS "${SANDBOX_LIBCRUN_SOURCE_DIR}/src")
find_path(LIBCRUN_CONFIG_DIR config.h HINTS "${SANDBOX_LIBCRUN_SOURCE_DIR}" NO_DEFAULT_PATH)
find_path(LIBCRUN_OCISPEC_INCLUDE_DIR ocispec/runtime_spec_schema_config_schema.h
  HINTS "${SANDBOX_LIBCRUN_SOURCE_DIR}/libocispec/src")
find_library(LIBCRUN_LIBRARY NAMES crun HINTS /usr/local/lib)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Libcrun
  REQUIRED_VARS LIBCRUN_LIBRARY LIBCRUN_INCLUDE_DIR LIBCRUN_CONFIG_DIR LIBCRUN_OCISPEC_INCLUDE_DIR)
if(Libcrun_FOUND AND NOT TARGET Libcrun::Libcrun)
  add_library(Libcrun::Libcrun UNKNOWN IMPORTED)
  set_target_properties(Libcrun::Libcrun PROPERTIES
    IMPORTED_LOCATION "${LIBCRUN_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${LIBCRUN_INCLUDE_DIR};${LIBCRUN_CONFIG_DIR};${LIBCRUN_OCISPEC_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES "PkgConfig::CRUN_SUPPORT;m")
endif()
