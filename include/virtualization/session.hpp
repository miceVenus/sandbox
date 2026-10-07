#pragma once

#include "virtualization/agentd_client.hpp"
#include <functional>
#include <mutex>

namespace virtualization {
    // One SDK-to-agentd communication channel. It never owns sandbox lifecycle.
    // A disconnected or unresponsive agentd does not prevent host-side reclamation.
    class Session {
      public:
        auto connect(const std::filesystem::path &socket, ipc::Limits limits,
                     std::string_view isolation, std::chrono::milliseconds timeout,
                     bool startup = false) -> std::shared_ptr<AgentdClient>;
        [[nodiscard]] auto is_connected() const -> bool;
        void disconnect();
        auto execute(const RuntimeCommand &command) -> Result;
        auto read(const std::filesystem::path &path, size_t limit) -> Result;
        auto write(const std::filesystem::path &path, std::string_view contents) -> Result;
        auto cancel() -> bool;
        void ping();
        void freeze_workspace(bool frozen);

      private:
        auto client() const -> std::shared_ptr<AgentdClient>;
        auto request(const std::function<Result(AgentdClient &)> &operation) -> Result;
        mutable std::mutex mutex_;
        std::shared_ptr<AgentdClient> client_;
        std::filesystem::path socket_;
    };
} // namespace virtualization
