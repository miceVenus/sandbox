#include "virtualization/session.hpp"
#include "lib/error.hpp"

#include <thread>

namespace virtualization {
    auto Session::connect(const std::filesystem::path &socket, ipc::Limits limits,
                          std::string_view isolation, std::chrono::milliseconds timeout,
                          bool startup) -> std::shared_ptr<AgentdClient> {
        std::scoped_lock lock(mutex_);
        if (client_) {
            lib::require(socket_ == socket &&
                             client_->runtime_info().value("isolation", "") == isolation,
                         "session is already connected to another agentd");
            return client_;
        }
        const auto deadline = ipc::Clock::now() + timeout;
        for (;;) {
            try {
                auto connected =
                    std::make_shared<AgentdClient>(ipc::connect_unix(socket, deadline), limits);
                connected->handshake();
                ipc::require(connected->runtime_info().value("isolation", "") == isolation,
                             "unexpected agentd runtime identity");
                socket_ = socket;
                client_ = std::move(connected);
                return client_;
            } catch (const ipc::TransportError &) {
                if (!startup || ipc::Clock::now() >= deadline) {
                    throw;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
    }

    auto Session::client() const -> std::shared_ptr<AgentdClient> {
        std::scoped_lock lock(mutex_);
        lib::require(client_ != nullptr, "session is not connected to agentd");
        return client_;
    }

    auto Session::is_connected() const -> bool {
        std::scoped_lock lock(mutex_);
        return client_ != nullptr;
    }

    void Session::disconnect() {
        std::scoped_lock lock(mutex_);
        client_.reset();
        socket_.clear();
    }

    auto Session::request(const std::function<Result(AgentdClient &)> &operation) -> Result {
        try {
            return operation(*client());
        } catch (const RemoteError &error) {
            Result result;
            result.runtime_status = 1;
            result.err = error.what();
            return result;
        } catch (const ipc::Timeout &error) {
            Result result;
            result.timed_out = true;
            result.err = error.what();
            return result;
        }
    }

    auto Session::execute(const RuntimeCommand &command) -> Result {
        return request([&](AgentdClient &agentd) {
            return agentd.execute(command);
        });
    }

    auto Session::read(const std::filesystem::path &path, size_t limit) -> Result {
        return request([&](AgentdClient &agentd) {
            Result result;
            result.out = agentd.read(path, limit);
            result.runtime_status = 0;
            return result;
        });
    }

    auto Session::write(const std::filesystem::path &path, std::string_view contents) -> Result {
        return request([&](AgentdClient &agentd) {
            agentd.write(path, contents);
            Result result;
            result.runtime_status = 0;
            return result;
        });
    }

    auto Session::cancel() -> bool {
        std::shared_ptr<AgentdClient> connected;
        {
            std::scoped_lock lock(mutex_);
            connected = client_;
        }
        return connected && connected->cancel();
    }

    void Session::ping() {
        client()->ping();
    }
    void Session::freeze_workspace(bool frozen) {
        client()->freeze_workspace(frozen);
    }
} // namespace virtualization
