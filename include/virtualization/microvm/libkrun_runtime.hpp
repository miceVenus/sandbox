#pragma once

#include "../runtime.hpp"
#include <chrono>

// Trusted service configuration. Agent-supplied commands cannot select host mounts,
// libraries, kernel images or launcher paths.
struct LibkrunConfig {
    unsigned vcpus = 1;
    std::chrono::milliseconds startup_timeout{30000};
};

// The VMM runs inside an SDK-owned libcrun boundary; commands run inside its Guest.
// Initial support: Linux/rootless, network disabled, HostTools or Minimal.
std::unique_ptr<RuntimeBackend> make_libkrun_backend(LibkrunConfig config = {});
