#pragma once
#include "sandbox_types.hpp"
#include <nlohmann/json.hpp>

// Shared enforcement for OCI workloads and an OCI-isolated VMM worker.
void check_rootless_cgroups(const Options &options);
void verify_runtime_resources(const SandboxInfo &info, const nlohmann::json &state);
