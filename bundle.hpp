#pragma once

struct Session;

// Initialize rootfs and workspace ownership, then write the OCI configuration.
void prepare_bundle(const Session &session);
