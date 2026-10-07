#pragma once

#include "virtualization/runtime.hpp"
#include <chrono>

// Trusted service configuration. Caller-supplied commands cannot select host mounts,
// libraries, kernel images or launcher paths.
struct KrunConfig {
    unsigned vcpus = 1;
    std::chrono::milliseconds startup_timeout{30000};
};

// The VMM runs inside an SDK-owned libcrun boundary; commands run inside its Guest.
// Initial support: Linux/rootless, network disabled, HostTools or Minimal.
auto make_krun_backend(KrunConfig config = {}) -> std::unique_ptr<RuntimeBackend>;
