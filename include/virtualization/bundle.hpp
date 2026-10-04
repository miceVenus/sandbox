#pragma once

struct SandboxInfo;

// Initialize rootfs and workspace ownership, then write the OCI configuration.
void prepare_bundle(const SandboxInfo &info);
