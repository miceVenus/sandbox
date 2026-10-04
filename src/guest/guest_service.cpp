#include "guest/guest_service.hpp"
#include "guest/guest_vm.hpp"
#include "virtualization/runtime.hpp"
#include "workspace/workspace_files.hpp"

#include <algorithm>
#include <atomic>
#include <sys/fsuid.h>
#include <thread>

namespace protocol {
    namespace {
        namespace fs = std::filesystem;

        class FileIdentity {
          public:
            explicit FileIdentity(bool mapped) : active_(mapped) {
                if (active_) {
                    previous_gid_ = setfsgid(65534);
                    previous_ = setfsuid(65534);
                    if (setfsuid(-1) != 65534 || setfsgid(-1) != 65534) {
                        setfsuid(previous_);
                        setfsgid(previous_gid_);
                        throw ProtocolError("cannot enter Guest file identity");
                    }
                }
            }
            ~FileIdentity() {
                if (active_) {
                    setfsuid(previous_);
                    setfsgid(previous_gid_);
                }
            }
          private:
            bool active_;
            int previous_ = 0;
            int previous_gid_ = 0;
        };

        struct Task {
            uint32_t id;
            RuntimeCommand command;
            std::atomic<bool> cancel{false};
            std::atomic<bool> done{false};
            std::thread worker;

            ~Task() {
                cancel = true;
                if (worker.joinable()) {
                    worker.join();
                }
            }
        };

        struct Upload {
            uint32_t id = 0;
            size_t expected = 0;
            size_t received = 0;
            std::unique_ptr<WorkspaceFiles::Write> writer;
            std::unique_ptr<Task> task;
            Deadline deadline;
        };

        class Service {
          public:
            Service(std::unique_ptr<Transport> transport, GuestConfig config)
                : channel_(std::move(transport)), config_(std::move(config)),
                  files_(config_.workspace), limits_(config_.limits) {
                require(config_.io_timeout.count() > 0 && config_.idle_timeout.count() > 0 &&
                            limits_.file_bytes > 0 && limits_.file_bytes <= 64 * 1024 * 1024 &&
                            limits_.stdin_bytes > 0 && limits_.stdin_bytes <= 64 * 1024 * 1024 &&
                            limits_.output_bytes > 0 && limits_.output_bytes <= 64 * 1024 * 1024 &&
                            limits_.timeout_ms > 0 && limits_.timeout_ms <= 3600000,
                        "invalid Guest configuration");
            }

            ~Service() {
                try {
                    discard_upload();
                } catch (...) {
                    channel_.invalidate();
                }
            }

            void run() {
                const auto hello = channel_.receive(Clock::now() + config_.io_timeout);
                require(hello.id == 0 && hello.flag == Flag::Request && hello.type == "core.hello",
                        "expected initial core.hello");
                handshake(hello.payload);
                reply(0,
                      Flag::Terminal,
                      "core.ready",
                      {{"protocol", protocol_name},
                       {"version", protocol_version},
                       {"file_bytes", limits_.file_bytes},
                       {"stdin_bytes", limits_.stdin_bytes},
                       {"output_bytes", limits_.output_bytes},
                       {"timeout_ms", limits_.timeout_ms},
                       {"runtime", config_.runtime_info},
                       {"capabilities",
                        Json::array({"exec", "exec.cancel", "fs.read", "fs.write"})}});
                for (;;) {
                    const auto until =
                        upload_ ? upload_->deadline
                                : Clock::now() + config_.idle_timeout +
                                      (task_ && !task_->done
                                           ? std::chrono::milliseconds(limits_.timeout_ms)
                                           : std::chrono::milliseconds(0));
                    const auto message = channel_.receive(until);
                    if (message.flag == Flag::Event) {
                        event(message);
                        continue;
                    }
                    require(message.flag == Flag::Request && message.id > last_request_ && !upload_,
                            "invalid request order or overlapping upload");
                    last_request_ = message.id;
                    if (task_) {
                        if (!task_->done) {
                            error(message.id, "busy", "Guest is executing another command");
                            continue;
                        }
                        task_.reset(); // Join the completed worker before reusing its state.
                    }
                    request(message);
                }
            }

          private:
            void discard_upload() {
                FileIdentity file_identity(!config_.task_cgroup.empty() && upload_ && upload_->writer);
                upload_.reset();
            }

            void reply(uint32_t id, Flag flag, const char *type, Json payload) {
                channel_.send({id, flag, type, std::move(payload)},
                              Clock::now() + config_.io_timeout);
            }
            void error(uint32_t id, const char *code, const std::string &message) {
                reply(id,
                      Flag::Terminal,
                      "core.error",
                      {{"code", code}, {"message", message.substr(0, 4096)}});
            }

            void handshake(const Json &payload) {
                require(text_field(payload, "protocol", 64) == protocol_name &&
                            unsigned_field(payload, "version", UINT32_MAX) == protocol_version,
                        "incompatible Host protocol");
                limits_.file_bytes = std::min<uint64_t>(
                    limits_.file_bytes, unsigned_field(payload, "file_bytes", 64 * 1024 * 1024));
                limits_.stdin_bytes = std::min<uint64_t>(
                    limits_.stdin_bytes, unsigned_field(payload, "stdin_bytes", 64 * 1024 * 1024));
                limits_.output_bytes =
                    std::min<uint64_t>(limits_.output_bytes,
                                       unsigned_field(payload, "output_bytes", 64 * 1024 * 1024));
                limits_.timeout_ms = std::min<uint64_t>(
                    limits_.timeout_ms, unsigned_field(payload, "timeout_ms", 3600000));
                require(limits_.file_bytes && limits_.stdin_bytes && limits_.output_bytes &&
                            limits_.timeout_ms,
                        "zero negotiated limits");
            }

            RuntimeCommand command(const Json &payload) {
                require(payload.contains("argv") && payload.at("argv").is_array() &&
                            !payload.at("argv").empty() && payload.at("argv").size() <= 1024,
                        "invalid command argv");
                RuntimeCommand result;
                size_t total = 0;
                for (const auto &argument : payload.at("argv")) {
                    require(argument.is_string(), "argv must contain text");
                    auto text = argument.get<std::string>();
                    require(text.find('\0') == std::string::npos, "NUL in argv");
                    total += text.size();
                    result.argv.push_back(std::move(text));
                }
                require(total <= 32 * 1024 && !result.argv[0].empty() && result.argv[0][0] == '/',
                        "expected an absolute executable path and bounded argv");
                result.cwd = text_field(payload, "cwd");
                require(result.cwd.is_absolute(), "cwd must be absolute");
                for (const auto &part : result.cwd) {
                    require(part != "..", "cwd traversal rejected");
                }
                const auto relative = result.cwd.lexically_normal().lexically_relative(
                    config_.workspace.lexically_normal());
                require(!relative.empty() && !relative.is_absolute(), "cwd is outside workspace");
                for (const auto &part : relative) {
                    require(part != "..", "cwd is outside workspace");
                }
                result.timeout_ms = unsigned_field(payload, "timeout_ms", limits_.timeout_ms);
                result.output_limit = unsigned_field(payload, "output_bytes", limits_.output_bytes);
                require(result.timeout_ms > 0 && result.output_limit > 0, "invalid command limits");
                return result;
            }

            void request(const Message &message) {
                try {
                    FileIdentity file_identity(!config_.task_cgroup.empty() &&
                                               message.type.compare(0, 3, "fs.") == 0);
                    if (message.type == "core.ping") {
                        reply(message.id, Flag::Terminal, "core.pong", Json::object());
                    } else if (message.type == "workspace.freeze") {
                        require(message.payload.at("frozen").is_boolean(), "invalid freeze value");
                        freeze_guest_workspace(config_, message.payload.at("frozen"));
                        reply(message.id, Flag::Terminal, "workspace.frozen", message.payload);
                    } else if (message.type == "exec.start") {
                        auto upload = std::make_unique<Upload>();
                        upload->id = message.id;
                        upload->expected =
                            unsigned_field(message.payload, "stdin_bytes", limits_.stdin_bytes);
                        upload->deadline = Clock::now() + config_.io_timeout;
                        upload->task = std::make_unique<Task>();
                        upload->task->id = message.id;
                        upload->task->command = command(message.payload);
                        upload_ = std::move(upload);
                        reply(message.id, Flag::Event, "exec.accepted", Json::object());
                    } else if (message.type == "fs.write") {
                        auto upload = std::make_unique<Upload>();
                        upload->id = message.id;
                        upload->expected =
                            unsigned_field(message.payload, "size", limits_.file_bytes);
                        upload->deadline = Clock::now() + config_.io_timeout;
                        upload->writer = files_.begin_write(text_field(message.payload, "path"),
                                                            limits_.file_bytes);
                        upload_ = std::move(upload);
                        reply(message.id, Flag::Event, "fs.write.accepted", Json::object());
                    } else if (message.type == "fs.read") {
                        const auto limit =
                            unsigned_field(message.payload, "limit", limits_.file_bytes);
                        require(limit > 0, "invalid read limit");
                        const auto content =
                            files_.read(text_field(message.payload, "path"), limit);
                        const auto until = Clock::now() + config_.io_timeout;
                        for (size_t offset = 0; offset < content.size(); offset += chunk_bytes) {
                            channel_.send({message.id,
                                           Flag::Event,
                                           "fs.read.data",
                                           {{"offset", offset},
                                            {"data",
                                             binary_bytes(std::string_view(content).substr(
                                                 offset, chunk_bytes))}}},
                                          until);
                        }
                        reply(
                            message.id, Flag::Terminal, "fs.read.done", {{"size", content.size()}});
                    } else {
                        error(message.id, "unsupported", "unsupported operation: " + message.type);
                    }
                } catch (const TransportError &) {
                    throw;
                } catch (const ProtocolError &failure) {
                    discard_upload();
                    error(message.id, "invalid_request", failure.what());
                } catch (const std::exception &failure) {
                    discard_upload();
                    error(message.id, "io_error", failure.what());
                }
            }

            void event(const Message &message) {
                FileIdentity file_identity(!config_.task_cgroup.empty() && upload_ && upload_->writer);
                if (message.type == "exec.cancel" && message.id != 0 && message.id == last_exec_) {
                    if (task_ && !task_->done) {
                        task_->cancel = true;
                    }
                    return; // A late cancellation never targets a subsequent request.
                }
                require(upload_ && message.id == upload_->id, "event does not belong to an upload");
                const bool exec = bool(upload_->task);
                const std::string data_type = exec ? "exec.stdin" : "fs.write.data";
                const std::string end_type = exec ? "exec.stdin.end" : "fs.write.end";
                if (message.type == data_type) {
                    require(unsigned_field(message.payload, "offset", upload_->expected) ==
                                upload_->received,
                            "invalid upload offset");
                    require(message.payload.contains("data"), "upload data is required");
                    const auto data = binary_string(message.payload.at("data"), chunk_bytes);
                    require(!data.empty() && data.size() <= upload_->expected - upload_->received,
                            "upload exceeds declared size");
                    if (exec) {
                        upload_->task->command.stdin_data += data;
                    } else {
                        upload_->writer->append(data);
                    }
                    upload_->received += data.size();
                    return;
                }
                require(message.type == end_type && upload_->received == upload_->expected,
                        "incomplete or invalid upload termination");
                if (exec) {
                    task_ = std::move(upload_->task);
                    last_exec_ = task_->id;
                    upload_.reset();
                    start_task();
                } else {
                    // Any exception here terminates the connection; no stream is resumed.
                    upload_->writer->commit();
                    const auto size = upload_->received;
                    upload_.reset();
                    reply(message.id, Flag::Terminal, "fs.write.done", {{"size", size}});
                }
            }

            void start_task() {
                Task *task = task_.get();
                task->worker = std::thread([this, task] {
                    try {
                        reply(task->id, Flag::Event, "exec.started", Json::object());
                        size_t stdout_offset = 0, stderr_offset = 0;
                        ProcessSupervision supervision;
                        supervision.cwd = task->command.cwd;
                        supervision.cancel = &task->cancel;
                        supervision.kill_remaining_group = true;
                        supervision.on_output = [&](bool stderr_stream, std::string_view bytes) {
                            auto &offset = stderr_stream ? stderr_offset : stdout_offset;
                            reply(task->id,
                                  Flag::Event,
                                  stderr_stream ? "exec.stderr" : "exec.stdout",
                                  {{"offset", offset}, {"data", binary_bytes(bytes)}});
                            offset += bytes.size();
                        };
                        auto argv = task->command.argv;
                        if (!config_.task_launcher.empty()) {
                            argv.insert(argv.begin(), {config_.task_launcher.string(),
                                                      "--run-task", "--"});
                        }
                        const auto result = run_process(argv,
                                                        task->command.timeout_ms,
                                                        task->command.output_limit,
                                                        true,
                                                        task->command.stdin_data,
                                                        supervision);
                        finish_guest_tasks(config_);
                        task->done = true;
                        reply(task->id,
                              Flag::Terminal,
                              "exec.exited",
                              {{"code", uint32_t(result.runtime_status)},
                               {"timed_out", result.timed_out},
                               {"output_limited", result.output_limited},
                               {"cancelled", result.cancelled}});
                    } catch (const std::exception &failure) {
                        try {
                            finish_guest_tasks(config_);
                        } catch (...) {
                            channel_.invalidate();
                        }
                        task->done = true;
                        try {
                            error(task->id, "exec_error", failure.what());
                        } catch (...) {
                            channel_.invalidate();
                        }
                    }
                });
            }

            // Declared before task_: the channel outlives cancellation/join during unwind.
            Channel channel_;
            GuestConfig config_;
            WorkspaceFiles files_;
            Limits limits_;
            std::unique_ptr<Upload> upload_;
            std::unique_ptr<Task> task_;
            uint32_t last_request_ = 0;
            uint32_t last_exec_ = 0;
        };
    } // namespace

    void serve_guest(std::unique_ptr<Transport> transport, const GuestConfig &config) {
        Service(std::move(transport), config).run();
    }
} // namespace protocol
