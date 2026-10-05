#include "communication/agent_connection.hpp"
#include <thread>

namespace protocol {
    std::shared_ptr<Client> AgentConnection::connect(const std::filesystem::path &socket,
                                                   Limits limits,
                                                   std::string_view isolation,
                                                   std::chrono::milliseconds timeout,
                                                   bool startup) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (client_) {
            return client_;
        }
        const auto until = Clock::now() + timeout;
        for (;;) {
            try {
                auto client = std::make_shared<Client>(connect_unix(socket, until), limits);
                client->handshake();
                require(client->runtime_info().value("isolation", "") == isolation,
                        "unexpected agent runtime identity");
                client_ = client;
                return client;
            } catch (const TransportError &) {
                if (!startup || Clock::now() >= until) {
                    throw;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
    }
    std::shared_ptr<Client> AgentConnection::current() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return client_;
    }
    void AgentConnection::clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        client_.reset();
    }
}
