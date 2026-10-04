find_package(nlohmann_json CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(libgit2 CONFIG REQUIRED)
find_package(Libcrun REQUIRED)

if(SANDBOX_ENABLE_LIBKRUN)
  find_package(Libkrun REQUIRED)
endif()

# Group internal runtime targets in IDEs which support target folders.
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
