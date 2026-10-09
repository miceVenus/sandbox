#include "virtualization/agentd_client.hpp"
#include "lib/error.hpp"
#include "ipc/socket.hpp"

#include <algorithm>
#include <mutex>
#include <thread>

namespace virtualization {
    using namespace ipc;

    RemoteError::RemoteError(std::string value, std::string message)
        : std::runtime_error(std::move(message)), code(std::move(value)) {
    }

    AgentdClient::AgentdClient(lib::UniqueFd descriptor, Limits requested,
                               std::chrono::milliseconds io_timeout)
        : session_(std::move(descriptor)), limits_(requested), io_timeout_(io_timeout) {
        validate_configuration();
    }

    void AgentdClient::validate_configuration() const {
        const auto io_timeout = io_timeout_;
        const auto requested = limits_;
        require(io_timeout.count() > 0 && io_timeout <= std::chrono::minutes(1),
                "invalid I/O timeout");
        require(requested.file_bytes > 0 && requested.file_bytes <= 64 * 1024 * 1024 &&
                    requested.stdin_bytes > 0 && requested.stdin_bytes <= 64 * 1024 * 1024 &&
                    requested.output_bytes > 0 && requested.output_bytes <= 64 * 1024 * 1024 &&
                    requested.timeout_ms > 0 && requested.timeout_ms <= 3600000,
                "invalid agentd limits");
    }

    void AgentdClient::connect(const std::filesystem::path &socket, Limits requested,
                              std::string_view isolation, std::chrono::milliseconds timeout,
                              bool startup) {
        std::scoped_lock operation(operations_);
        if (session_.is_connected()) {
            lib::require(socket_ == socket && ready_ &&
                             runtime_info_.value("isolation", "") == isolation,
                         "client is already connected to another agentd");
            return;
        }
        limits_ = requested;
        validate_configuration();
        const auto until = ipc::Clock::now() + timeout;
        for (;;) {
            try {
                session_.attach(ipc::connect_unix(socket, until));
                handshake_locked();
                require(runtime_info_.value("isolation", "") == isolation,
                        "unexpected agentd runtime identity");
                socket_ = socket;
                return;
            } catch (const ipc::IoError &) {
                session_.disconnect();
                ready_ = false;
                limits_ = requested;
                if (!startup || ipc::Clock::now() >= until) {
                    throw;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            } catch (...) {
                session_.disconnect();
                ready_ = false;
                throw;
            }
        }
    }

    void AgentdClient::disconnect() {
        std::scoped_lock operation(operations_);
        std::scoped_lock cancellation(cancellation_);
        session_.disconnect();
        ready_ = false;
        active_exec_ = last_id_ = 0;
        socket_.clear();
        runtime_info_ = Json::object();
    }

    auto AgentdClient::is_connected() const -> bool {
        return session_.is_connected();
    }

    auto AgentdClient::request_result(const std::function<Result()> &operation) -> Result {
        try {
            return operation();
        } catch (const RemoteError &error) {
            Result result;
            result.runtime_status = 1;
            result.err = error.what();
            return result;
        } catch (const ipc::IoTimeout &error) {
            Result result;
            result.timed_out = true;
            result.err = error.what();
            return result;
        }
    }

    auto AgentdClient::execute_result(const RuntimeCommand &command) -> Result {
        return request_result([&] { return execute(command); });
    }

    auto AgentdClient::read_result(const std::filesystem::path &path, size_t limit) -> Result {
        return request_result([&] {
            Result result;
            result.out = read(path, limit);
            result.runtime_status = 0;
            return result;
        });
    }

    auto AgentdClient::write_result(const std::filesystem::path &path,
                                   std::string_view content) -> Result {
        return request_result([&] {
            write(path, content);
            Result result;
            result.runtime_status = 0;
            return result;
        });
    }

    auto AgentdClient::deadline() const -> ipc::Deadline {
        return ipc::Clock::now() + io_timeout_;
    }
    void AgentdClient::require_ready() const {
        require(ready_, "agentd handshake is required");
    }
    auto AgentdClient::next_id() -> uint32_t {
        require_ready();
        require(last_id_ < UINT32_MAX, "request ID space exhausted; create a new connection");
        return ++last_id_;
    }

    auto AgentdClient::receive(uint32_t id, ipc::Deadline until) -> Message {
        auto message = session_.receive(until);
        try {
            require(message.id == id && message.flag != Flag::Request,
                    "unexpected response ID or flag");
            if (message.type == "core.error") {
                require(message.flag == Flag::Terminal, "error must end the operation");
                throw RemoteError(text_field(message.payload, "code", 64),
                                  text_field(message.payload, "message", 4096));
            }
            return message;
        } catch (const ProtocolError &) {
            session_.invalidate();
            throw;
        }
    }

    auto AgentdClient::expect(uint32_t id, Flag flag, const char *type, ipc::Deadline until) -> Message {
        auto message = receive(id, until);
        if (message.flag != flag || message.type != type) {
            session_.invalidate();
            throw ProtocolError("unexpected operation response: " + message.type);
        }
        return message;
    }

    void AgentdClient::handshake() {
        std::scoped_lock operation(operations_);
        handshake_locked();
    }

    void AgentdClient::handshake_locked() {
        require(!ready_, "agentd handshake already completed");
        const auto until = deadline();
        session_.send({0,
                       Flag::Request,
                       "core.hello",
                       {{"protocol", protocol_name},
                        {"version", protocol_version},
                        {"file_bytes", limits_.file_bytes},
                        {"stdin_bytes", limits_.stdin_bytes},
                        {"output_bytes", limits_.output_bytes},
                        {"timeout_ms", limits_.timeout_ms}}},
                      until);
        const auto response = expect(0, Flag::Terminal, "core.ready", until);
        try {
            const auto &p = response.payload;
            require(text_field(p, "protocol", 64) == protocol_name &&
                        unsigned_field(p, "version", UINT32_MAX) == protocol_version,
                    "incompatible agentd service");
            limits_.file_bytes = unsigned_field(p, "file_bytes", limits_.file_bytes);
            limits_.stdin_bytes = unsigned_field(p, "stdin_bytes", limits_.stdin_bytes);
            limits_.output_bytes = unsigned_field(p, "output_bytes", limits_.output_bytes);
            limits_.timeout_ms = unsigned_field(p, "timeout_ms", limits_.timeout_ms);
            require(limits_.file_bytes && limits_.stdin_bytes && limits_.output_bytes &&
                        limits_.timeout_ms,
                    "agentd negotiated zero limits");
            require(p.contains("capabilities") && p.at("capabilities").is_array(),
                    "missing capabilities");
            for (const auto *capability : {"exec", "fs.read", "fs.write", "exec.cancel"}) {
                require(std::find(p.at("capabilities").begin(), p.at("capabilities").end(),
                                  Json(capability)) != p.at("capabilities").end(),
                        "agentd lacks a required capability");
            }
            runtime_info_ = p.value("runtime", Json::object());
            ready_ = true;
        } catch (...) {
            session_.invalidate();
            throw;
        }
    }

    void AgentdClient::freeze_workspace(bool frozen) {
        std::scoped_lock operation(operations_);
        const auto id = next_id();
        const auto until = deadline();
        session_.send({id, Flag::Request, "workspace.freeze", {{"frozen", frozen}}}, until);
        const auto response = expect(id, Flag::Terminal, "workspace.frozen", until);
        require(response.payload.at("frozen") == frozen, "agentd freeze acknowledgement mismatch");
    }

    void AgentdClient::send_data(uint32_t id, const char *type, std::string_view bytes,
                                 ipc::Deadline until) {
        size_t offset = 0;
        while (!bytes.empty()) {
            const auto count = std::min(bytes.size(), chunk_bytes);
            session_.send({id,
                           Flag::Event,
                           type,
                           {{"offset", offset}, {"data", binary_bytes(bytes.substr(0, count))}}},
                          until);
            offset += count;
            bytes.remove_prefix(count);
        }
    }

    auto AgentdClient::execute(const RuntimeCommand &command,
                               const std::function<void(bool, std::string_view)> &on_output)
        -> Result {
        std::scoped_lock operation(operations_);
        require_ready();
        require(!command.argv.empty() && command.argv.size() <= 1024 &&
                    command.argv[0].size() > 0 && command.argv[0][0] == '/' &&
                    command.cwd.is_absolute(),
                "invalid command or cwd");
        size_t argv_bytes = 0;
        for (const auto &arg : command.argv) {
            require(arg.find('\0') == std::string::npos, "NUL in command argument");
            argv_bytes += arg.size();
        }
        require(argv_bytes <= 32 * 1024 && command.cwd.string().find('\0') == std::string::npos &&
                    command.stdin_data.size() <= limits_.stdin_bytes && command.timeout_ms > 0 &&
                    uint32_t(command.timeout_ms) <= limits_.timeout_ms &&
                    command.output_limit > 0 && command.output_limit <= limits_.output_bytes,
                "command exceeds negotiated limits");
        const auto id = next_id();
        const auto transfer_until = deadline();
        session_.send({id,
                       Flag::Request,
                       "exec.start",
                       {{"argv", command.argv},
                        {"cwd", command.cwd.string()},
                        {"stdin_bytes", command.stdin_data.size()},
                        {"timeout_ms", uint32_t(command.timeout_ms)},
                        {"output_bytes", command.output_limit}}},
                      transfer_until);
        expect(id, Flag::Event, "exec.accepted", transfer_until);
        send_data(id, "exec.stdin", command.stdin_data, transfer_until);
        session_.send({id, Flag::Event, "exec.stdin.end", Json::object()}, transfer_until);
        const auto execution_until =
            ipc::Clock::now() + std::chrono::milliseconds(command.timeout_ms) + io_timeout_;
        expect(id, Flag::Event, "exec.started", execution_until);
        {
            std::scoped_lock lock(cancellation_);
            active_exec_ = id;
        }
        struct ActiveReset {
            AgentdClient &client;
            ~ActiveReset() {
                std::scoped_lock lock(client.cancellation_);
                client.active_exec_ = 0;
            }
        } reset{*this};
        Result result;
        try {
            for (;;) {
                const auto message = receive(id, execution_until);
                if (message.flag == Flag::Terminal && message.type == "exec.exited") {
                    result.runtime_status =
                        static_cast<int>(unsigned_field(message.payload, "code", 255));
                    for (const auto *key : {"timed_out", "output_limited", "cancelled"}) {
                        require(message.payload.contains(key) &&
                                    message.payload.at(key).is_boolean(),
                                "invalid exit flags");
                    }
                    result.timed_out = message.payload.at("timed_out");
                    result.output_limited = message.payload.at("output_limited");
                    result.cancelled = message.payload.at("cancelled");
                    return result;
                }
                require(message.flag == Flag::Event &&
                            (message.type == "exec.stdout" || message.type == "exec.stderr"),
                        "unexpected exec event");
                auto &target = message.type == "exec.stdout" ? result.out : result.err;
                require(unsigned_field(message.payload, "offset", command.output_limit) ==
                            target.size(),
                        "invalid output offset");
                require(message.payload.contains("data"), "output data is required");
                const auto data = binary_string(message.payload.at("data"), chunk_bytes);
                require(data.size() <= command.output_limit - result.out.size() - result.err.size(),
                        "agentd exceeded output limit");
                target += data;
                if (on_output || command.on_output) {
                    try {
                        const bool stderr_stream = message.type == "exec.stderr";
                        if (on_output) {
                            on_output(stderr_stream, data);
                        } else {
                            command.on_output(
                                stderr_stream ? OutputStream::Stderr : OutputStream::Stdout, data);
                        }
                    } catch (...) {
                        session_.invalidate();
                        throw;
                    }
                }
            }
        } catch (const ProtocolError &) {
            session_.invalidate();
            throw;
        }
    }

    auto AgentdClient::cancel() -> bool {
        std::scoped_lock lock(cancellation_);
        if (!active_exec_) {
            return false;
        }
        session_.send({active_exec_, Flag::Event, "exec.cancel", Json::object()}, deadline());
        return true;
    }

    auto AgentdClient::read(const std::filesystem::path &path, size_t limit) -> std::string {
        std::scoped_lock operation(operations_);
        require(limit > 0 && limit <= limits_.file_bytes, "invalid read limit");
        const auto id = next_id();
        const auto until = deadline();
        session_.send({id, Flag::Request, "fs.read", {{"path", path.string()}, {"limit", limit}}},
                      until);
        std::string content;
        try {
            for (;;) {
                const auto message = receive(id, until);
                if (message.flag == Flag::Terminal && message.type == "fs.read.done") {
                    require(unsigned_field(message.payload, "size", limit) == content.size(),
                            "read size mismatch");
                    return content;
                }
                require(message.flag == Flag::Event && message.type == "fs.read.data",
                        "unexpected read event");
                require(unsigned_field(message.payload, "offset", limit) == content.size(),
                        "invalid file offset");
                require(message.payload.contains("data"), "file data is required");
                const auto data = binary_string(message.payload.at("data"), chunk_bytes);
                require(data.size() <= limit - content.size(), "agentd exceeded read limit");
                content += data;
            }
        } catch (const ProtocolError &) {
            session_.invalidate();
            throw;
        }
    }

    void AgentdClient::write(const std::filesystem::path &path, std::string_view content) {
        std::scoped_lock operation(operations_);
        require(content.size() <= limits_.file_bytes, "file exceeds negotiated limit");
        const auto id = next_id();
        const auto until = deadline();
        session_.send(
            {id, Flag::Request, "fs.write", {{"path", path.string()}, {"size", content.size()}}},
            until);
        expect(id, Flag::Event, "fs.write.accepted", until);
        send_data(id, "fs.write.data", content, until);
        session_.send({id, Flag::Event, "fs.write.end", Json::object()}, until);
        const auto response = expect(id, Flag::Terminal, "fs.write.done", until);
        try {
            require(unsigned_field(response.payload, "size", limits_.file_bytes) == content.size(),
                    "write size mismatch");
        } catch (...) {
            session_.invalidate();
            throw;
        }
    }

    void AgentdClient::ping() {
        std::scoped_lock operation(operations_);
        const auto id = next_id();
        const auto until = deadline();
        session_.send({id, Flag::Request, "core.ping", Json::object()}, until);
        expect(id, Flag::Terminal, "core.pong", until);
    }
} // namespace virtualization
