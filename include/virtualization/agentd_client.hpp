#pragma once

#include "ipc/transport.hpp"
#include "virtualization/runtime.hpp"
#include <mutex>

namespace virtualization {
    struct RemoteError : std::runtime_error {
        std::string code;
        RemoteError(std::string code, std::string message);
    };

    // First version serializes operations. cancel() may be called from another thread
    // while execute() is receiving output. Connection loss never replays an operation.
    class AgentdClient {
      public:
        explicit AgentdClient(std::unique_ptr<ipc::Transport> transport, ipc::Limits requested = {},
                              std::chrono::milliseconds io_timeout = std::chrono::seconds(5));
        void handshake();
        auto execute(const RuntimeCommand &command,
                     const std::function<void(bool, std::string_view)> &on_output = {}) -> Result;
        auto read(const std::filesystem::path &path, size_t limit) -> std::string;
        void write(const std::filesystem::path &path, std::string_view content);
        auto cancel() -> bool;
        void ping();
        void freeze_workspace(bool frozen);
        [[nodiscard]] auto runtime_info() const -> const ipc::Json & {
            return runtime_info_;
        }
        [[nodiscard]] auto limits() const -> const ipc::Limits & {
            return limits_;
        }

      private:
        [[nodiscard]] auto deadline() const -> ipc::Deadline;
        auto next_id() -> uint32_t;
        auto receive(uint32_t id, ipc::Deadline deadline) -> ipc::Message;
        auto expect(uint32_t id, ipc::Flag flag, const char *type, ipc::Deadline deadline)
            -> ipc::Message;
        void send_data(uint32_t id, const char *type, std::string_view bytes,
                       ipc::Deadline deadline);
        void require_ready() const;

        ipc::Channel channel_;
        ipc::Limits limits_;
        std::chrono::milliseconds io_timeout_;
        std::mutex operations_;
        std::mutex cancellation_;
        uint32_t active_exec_ = 0;
        uint32_t last_id_ = 0;
        bool ready_ = false;
        ipc::Json runtime_info_ = ipc::Json::object();
    };
} // namespace virtualization
