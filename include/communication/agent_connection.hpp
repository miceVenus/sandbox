#pragma once

#include "agent_client.hpp"

namespace protocol {
    // One persistent task-service connection per runtime. Only startup handshakes
    // may be retried; an operation is never replayed after a broken connection.
    class AgentConnection {
      public:
        std::shared_ptr<Client> connect(const std::filesystem::path &socket,
                                       Limits limits,
                                       std::string_view isolation,
                                       std::chrono::milliseconds timeout,
                                       bool startup = false);
        std::shared_ptr<Client> current() const;
        void clear();
      private:
        mutable std::mutex mutex_;
        std::shared_ptr<Client> client_;
    };
}
