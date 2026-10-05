#pragma once

#include "communication/agent_transport.hpp"
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
        Result execute(const RuntimeCommand &command,
                       const std::function<void(bool, std::string_view)> &on_output = {});
        std::string read(const std::filesystem::path &path, size_t limit);
        void write(const std::filesystem::path &path, std::string_view content);
        bool cancel();
        void ping();
        void freeze_workspace(bool frozen);
        const Json &runtime_info() const {
            return runtime_info_;
        }
        const Limits &limits() const {
            return limits_;
        }

      private:
        Deadline deadline() const;
        uint32_t next_id();
        Message receive(uint32_t id, Deadline deadline);
        Message expect(uint32_t id, Flag flag, const char *type, Deadline deadline);
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
