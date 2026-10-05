# All components come from one prefix. Switching it also replaces cached search results.
set(LIBKRUN_ROOT "" CACHE PATH "libkrun + libkrunfw development prefix")
if(NOT LIBKRUN_ROOT)
  set(LIBKRUN_ROOT "${SANDBOX_DEPS_PREFIX}")
endif()
get_filename_component(LIBKRUN_ROOT "${LIBKRUN_ROOT}" ABSOLUTE)
foreach(variable LIBKRUN_LIBRARY LIBKRUN_INCLUDE_DIR LIBKRUNFW_LIBRARY)
  unset(${variable} CACHE)
  unset(${variable})
endforeach()
find_path(LIBKRUN_INCLUDE_DIR libkrun.h
  PATHS "${LIBKRUN_ROOT}/include" NO_DEFAULT_PATH)
find_library(LIBKRUN_LIBRARY NAMES krun
  PATHS "${LIBKRUN_ROOT}/lib" "${LIBKRUN_ROOT}/lib64" NO_DEFAULT_PATH)
find_library(LIBKRUNFW_LIBRARY NAMES krunfw
  PATHS "${LIBKRUN_ROOT}/lib" "${LIBKRUN_ROOT}/lib64" NO_DEFAULT_PATH)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Libkrun
  REQUIRED_VARS LIBKRUN_INCLUDE_DIR LIBKRUN_LIBRARY LIBKRUNFW_LIBRARY
  REASON_FAILURE_MESSAGE "Run tools/build-deps.sh or set LIBKRUN_ROOT. Both libraries and their headers must share this prefix")
if(Libkrun_FOUND AND NOT TARGET Libkrun::Libkrun)
  add_library(Libkrun::Libkrun SHARED IMPORTED)
  set_target_properties(Libkrun::Libkrun PROPERTIES
    IMPORTED_LOCATION "${LIBKRUN_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${LIBKRUN_INCLUDE_DIR}")
endif()
