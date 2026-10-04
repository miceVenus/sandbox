# The dependency is private to the SDK's VMM worker, never linked into callers.
set(LIBKRUN_ROOT "" CACHE PATH "libkrun + libkrunfw installation prefix")
find_path(LIBKRUN_INCLUDE_DIR libkrun.h
  HINTS "${LIBKRUN_ROOT}" "$ENV{KRUN_PREFIX}" "$ENV{HOME}/.local/opt/libkrun"
  PATH_SUFFIXES include)
find_library(LIBKRUN_LIBRARY NAMES krun
  HINTS "${LIBKRUN_ROOT}" "$ENV{KRUN_PREFIX}" "$ENV{HOME}/.local/opt/libkrun"
  PATH_SUFFIXES lib lib64)
find_library(LIBKRUNFW_LIBRARY NAMES krunfw
  HINTS "${LIBKRUN_ROOT}" "$ENV{KRUN_PREFIX}" "$ENV{HOME}/.local/opt/libkrun"
  PATH_SUFFIXES lib lib64)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Libkrun
  REQUIRED_VARS LIBKRUN_INCLUDE_DIR LIBKRUN_LIBRARY LIBKRUNFW_LIBRARY)
if(Libkrun_FOUND AND NOT TARGET Libkrun::Libkrun)
  add_library(Libkrun::Libkrun UNKNOWN IMPORTED)
  set_target_properties(Libkrun::Libkrun PROPERTIES
    IMPORTED_LOCATION "${LIBKRUN_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${LIBKRUN_INCLUDE_DIR}")
endif()
