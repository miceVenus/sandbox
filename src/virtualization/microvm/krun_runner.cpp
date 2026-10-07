// Internal VMM worker. This process runs inside the Host OCI isolation boundary.
#include <fstream>
#include <iostream>
#include <libkrun.h>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace {
    void check(int result, const char *operation) {
        if (result < 0) {
            throw std::runtime_error(std::string(operation) + ": " + std::to_string(result));
        }
    }
} // namespace

auto main(int argc, char **argv) -> int {
    try {
        if (argc != 2) {
            throw std::runtime_error("sandbox-krun requires its private configuration");
        }
        std::ifstream input(argv[1]);
        const auto config = nlohmann::json::parse(input);
        const int ctx = krun_create_ctx();
        check(ctx, "create context");
        check(krun_set_vm_config(ctx, config.at("vcpus"), config.at("ram_mib")), "VM config");
        // Disable automatic transparent host socket impersonation explicitly.
        check(krun_disable_implicit_vsock(ctx), "disable implicit vsock");
        check(krun_add_vsock(ctx, 0), "add IPC-only vsock");
        check(krun_add_vsock_port2(ctx, 10789, "/control/agentd.sock", true), "vsock mapping");
        check(krun_add_virtiofs4(ctx, KRUN_FS_ROOT_TAG, "/guest-root", 0, true,
                                 KRUN_SEMANTICS_LINUX_SIMPLIFIED),
              "readonly Guest root");
        check(krun_add_virtiofs4(ctx, "sandbox-workspace", "/shares/workspace", 0, false,
                                 KRUN_SEMANTICS_LINUX_COMPLETE),
              "workspace share");
        check(krun_add_virtiofs4(ctx, "sandbox-data", "/shares/data", 0, false,
                                 KRUN_SEMANTICS_LINUX_COMPLETE),
              "runtime data share");
        check(krun_set_workdir(ctx, "/"), "Guest workdir");
        check(krun_set_console_output(ctx, "/control/console.log"), "console output");
        const std::string workspace = config.at("workspace");
        const char *args[] = {"--serve", "--workspace", workspace.c_str(),           "--vsock-port",
                              "10789",   "--vm-config", "/sandbox-tools/guest.json", nullptr};
        const char *env[] = {"PATH=/usr/bin:/bin", "LANG=C", "HOME=/", nullptr};
        check(krun_set_exec(ctx, "/sandbox-tools/agentd", args, env), "Guest service");
        check(krun_start_enter(ctx), "VM startup");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "sandbox-krun: " << error.what() << '\n';
        return 1;
    }
}
