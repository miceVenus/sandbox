#pragma once
#include "guest_service.hpp"

namespace protocol {
    void configure_guest_vm(GuestConfig &config, const std::filesystem::path &settings);
    void finish_guest_tasks(const GuestConfig &config);
    void freeze_guest_workspace(const GuestConfig &config, bool frozen);
}
