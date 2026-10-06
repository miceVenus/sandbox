#pragma once

#include "ipc/transport.hpp"
#include "virtualization/runtime.hpp"
#include <mutex>

namespace protocol {
    struct RemoteError : std::runtime_error {
        std::string code;
        RemoteError(std::string code, std::string message);
    };

    // First version serializes operations. cancel() may be called from another thread
    // while execute() is receiving output. Connection loss never replays an operation.
    class Client {
      public:
        explicit Client(std::unique_ptr<Transport> transport,
                        Limits requested = {},
                        std::chrono::milliseconds io_timeout = std::chrono::seconds(5));
        void handshake();
        auto execute(const RuntimeCommand &command,
                       const std::function<void(bool, std::string_view)> &on_output = {}) -> Result;
        auto read(const std::filesystem::path &path, size_t limit) -> std::string;
        void write(const std::filesystem::path &path, std::string_view content);
        auto cancel() -> bool;
        void ping();
        void freeze_workspace(bool frozen);
        [[nodiscard]] auto runtime_info() const -> const Json & {
            return runtime_info_;
        }
        [[nodiscard]] auto limits() const -> const Limits & {
            return limits_;
        }

      private:
        [[nodiscard]] auto deadline() const -> Deadline;
        auto next_id() -> uint32_t;
        auto receive(uint32_t id, Deadline deadline) -> Message;
        auto expect(uint32_t id, Flag flag, const char *type, Deadline deadline) -> Message;
        void send_data(uint32_t id, const char *type, std::string_view bytes, Deadline deadline);
        void require_ready() const;

        Channel channel_;
        Limits limits_;
        std::chrono::milliseconds io_timeout_;
        std::mutex operations_;
        std::mutex cancellation_;
        uint32_t active_exec_ = 0;
        uint32_t last_id_ = 0;
        bool ready_ = false;
        Json runtime_info_ = Json::object();
    };
} // namespace protocol
