# A development prefix contains one matching static library, OCI headers and
# generated configuration. Never pair a system archive with unrelated source headers.
set(LIBCRUN_ROOT "" CACHE PATH "Complete libcrun development prefix")
if(NOT LIBCRUN_ROOT)
  set(LIBCRUN_ROOT "${SANDBOX_DEPS_PREFIX}")
endif()
get_filename_component(LIBCRUN_ROOT "${LIBCRUN_ROOT}" ABSOLUTE)
foreach(variable LIBCRUN_LIBRARY LIBCRUN_INCLUDE_DIR LIBCRUN_CONFIG_DIR LIBCRUN_OCISPEC_INCLUDE_DIR)
  unset(${variable} CACHE)
  unset(${variable})
endforeach()
find_path(LIBCRUN_INCLUDE_DIR libcrun/container.h
  PATHS "${LIBCRUN_ROOT}/include" NO_DEFAULT_PATH)
find_path(LIBCRUN_CONFIG_DIR config.h
  PATHS "${LIBCRUN_ROOT}/include/bbm-sandbox-deps/crun" NO_DEFAULT_PATH)
find_path(LIBCRUN_OCISPEC_INCLUDE_DIR ocispec/runtime_spec_schema_config_schema.h
  PATHS "${LIBCRUN_ROOT}/include" NO_DEFAULT_PATH)
find_file(LIBCRUN_LIBRARY libcrun.a
  PATHS "${LIBCRUN_ROOT}/lib" "${LIBCRUN_ROOT}/lib64" NO_DEFAULT_PATH)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CRUN_SUPPORT REQUIRED IMPORTED_TARGET libsystemd libseccomp libcap json-c)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Libcrun
  REQUIRED_VARS LIBCRUN_LIBRARY LIBCRUN_INCLUDE_DIR LIBCRUN_CONFIG_DIR LIBCRUN_OCISPEC_INCLUDE_DIR
  REASON_FAILURE_MESSAGE "Run tools/build-deps.sh, or set LIBCRUN_ROOT to a complete development prefix. See doc/dependencies.md")
if(Libcrun_FOUND AND NOT TARGET Libcrun::Libcrun)
  add_library(Libcrun::Libcrun STATIC IMPORTED)
  set_target_properties(Libcrun::Libcrun PROPERTIES
    IMPORTED_LOCATION "${LIBCRUN_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${LIBCRUN_INCLUDE_DIR};${LIBCRUN_CONFIG_DIR};${LIBCRUN_OCISPEC_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES "PkgConfig::CRUN_SUPPORT;m;${CMAKE_DL_LIBS}")
endif()
