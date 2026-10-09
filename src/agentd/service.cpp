#include "agentd/service.hpp"
#include "agentd/workspace_files.hpp"
#include "ipc/session.hpp"
#include "lib/process.hpp"

#include <algorithm>
#include <atomic>
#include <optional>
#include <thread>

namespace agentd {
    using namespace ipc;
    namespace {
        namespace fs = std::filesystem;

        struct Task {
            uint32_t id;
            std::vector<std::string> argv;
            std::string stdin_data;
            lib::ProcessOptions options;
            std::atomic<bool> cancel{false};
            std::atomic<bool> finished{false};
            std::thread worker;

            ~Task() {
                cancel = true;
                if (worker.joinable()) {
                    worker.join();
                }
            }
        };

        struct Upload {
            enum class Kind { Stdin, File };
            Kind kind;
            uint32_t id = 0;
            size_t expected = 0;
            size_t received = 0;
            std::unique_ptr<WorkspaceFiles::Write> writer;
            ipc::Deadline deadline;
        };

        class Service {
          public:
            Service(lib::UniqueFd descriptor, ServiceConfig config)
                : session_(std::move(descriptor)), config_(std::move(config)),
                  files_(config_.workspace, config_.mapped_file_identity), limits_(config_.limits) {
                require(config_.io_timeout.count() > 0 && config_.idle_timeout.count() >= 0 &&
                            limits_.file_bytes > 0 && limits_.file_bytes <= 64 * 1024 * 1024 &&
                            limits_.stdin_bytes > 0 && limits_.stdin_bytes <= 64 * 1024 * 1024 &&
                            limits_.output_bytes > 0 && limits_.output_bytes <= 64 * 1024 * 1024 &&
                            limits_.timeout_ms > 0 && limits_.timeout_ms <= 3600000,
                        "invalid agentd configuration");
            }

            void run() {
                const auto hello = session_.receive(ipc::Clock::now() + config_.io_timeout);
                require(hello.id == 0 && hello.flag == Flag::Request && hello.type == "core.hello",
                        "expected initial core.hello");
                handshake(hello.payload);
                reply(0, Flag::Terminal, "core.ready",
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
                        : config_.idle_timeout.count() == 0
                            ? ipc::Deadline::max()
                            : ipc::Clock::now() + config_.idle_timeout +
                                  (task_ && !task_->finished
                                       ? std::chrono::milliseconds(limits_.timeout_ms)
                                       : std::chrono::milliseconds(0));
                    const auto message = session_.receive(until);
                    if (message.flag == Flag::Event) {
                        event(message);
                        continue;
                    }

                    require(message.flag == Flag::Request && message.id > last_request_ && !upload_,
                            "invalid request order or overlapping upload");

                    last_request_ = message.id;
                    if (task_) {
                        if (!task_->finished) {
                            error(message.id, "busy", "agentd is executing another command");
                            continue;
                        }
                        task_.reset(); // Join the completed worker before reusing its state.
                    }
                    request(message);
                }
            }

          private:
            void discard_upload() {
                if (upload_ && upload_->kind == Upload::Kind::Stdin) {
                    task_.reset();
                }
                upload_.reset();
            }

            void reply(uint32_t id, Flag flag, const char *type, Json payload) {
                session_.send({id, flag, type, std::move(payload)},
                              ipc::Clock::now() + config_.io_timeout);
            }
            void error(uint32_t id, const char *code, const std::string &message) {
                reply(id, Flag::Terminal, "core.error",
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

            auto parse_task(uint32_t id, const Json &payload) -> std::unique_ptr<Task> {
                require(payload.contains("argv") && payload.at("argv").is_array() &&
                            !payload.at("argv").empty() && payload.at("argv").size() <= 1024,
                        "invalid command argv");
                auto task = std::make_unique<Task>();
                task->id = id;
                size_t total = 0;
                for (const auto &argument : payload.at("argv")) {
                    require(argument.is_string(), "argv must contain text");
                    auto text = argument.get<std::string>();
                    require(text.find('\0') == std::string::npos, "NUL in argv");
                    total += text.size();
                    task->argv.push_back(std::move(text));
                }
                require(total <= 32 * 1024 && !task->argv[0].empty() && task->argv[0][0] == '/',
                        "expected an absolute executable path and bounded argv");
                task->options.cwd = text_field(payload, "cwd");
                require(task->options.cwd.is_absolute(), "cwd must be absolute");
                for (const auto &part : task->options.cwd) {
                    require(part != "..", "cwd traversal rejected");
                }
                const auto relative = task->options.cwd.lexically_normal().lexically_relative(
                    config_.workspace.lexically_normal());
                require(!relative.empty() && !relative.is_absolute(), "cwd is outside workspace");
                for (const auto &part : relative) {
                    require(part != "..", "cwd is outside workspace");
                }
                task->options.timeout_ms = unsigned_field(payload, "timeout_ms", limits_.timeout_ms);
                task->options.output_limit =
                    unsigned_field(payload, "output_bytes", limits_.output_bytes);
                require(task->options.timeout_ms > 0 && task->options.output_limit > 0,
                        "invalid command limits");
                task->options.environment = config_.task_environment;
                task->options.cancel = &task->cancel;
                task->options.kill_remaining_group = true;
                return task;
            }

            void request(const Message &message) {
                try {
                    if (message.type == "core.ping") {
                        reply(message.id, Flag::Terminal, "core.pong", Json::object());
                    } else if (message.type == "workspace.freeze") {
                        require(message.payload.at("frozen").is_boolean(), "invalid freeze value");
                        require(bool(config_.freeze_workspace), "workspace freeze is unavailable");
                        config_.freeze_workspace(message.payload.at("frozen"));
                        reply(message.id, Flag::Terminal, "workspace.frozen", message.payload);
                    } else if (message.type == "exec.start") {
                        Upload upload{};
                        upload.id = message.id;
                        upload.expected =
                            unsigned_field(message.payload, "stdin_bytes", limits_.stdin_bytes);
                        upload.deadline = ipc::Clock::now() + config_.io_timeout;
                        upload.kind = Upload::Kind::Stdin;
                        task_ = parse_task(message.id, message.payload);
                        upload_ = std::move(upload);
                        reply(message.id, Flag::Event, "exec.accepted", Json::object());
                    } else if (message.type == "fs.write") {
                        Upload upload{};
                        upload.id = message.id;
                        upload.expected =
                            unsigned_field(message.payload, "size", limits_.file_bytes);
                        upload.deadline = ipc::Clock::now() + config_.io_timeout;
                        upload.kind = Upload::Kind::File;
                        upload.writer = files_.begin_write(text_field(message.payload, "path"),
                                                           limits_.file_bytes);
                        upload_ = std::move(upload);
                        reply(message.id, Flag::Event, "fs.write.accepted", Json::object());
                    } else if (message.type == "fs.read") {
                        const auto limit =
                            unsigned_field(message.payload, "limit", limits_.file_bytes);
                        require(limit > 0, "invalid read limit");
                        const auto content =
                            files_.read(text_field(message.payload, "path"), limit);
                        const auto until = ipc::Clock::now() + config_.io_timeout;
                        for (size_t offset = 0; offset < content.size(); offset += chunk_bytes) {
                            session_.send({message.id,
                                           Flag::Event,
                                           "fs.read.data",
                                           {{"offset", offset},
                                            {"data", binary_bytes(std::string_view(content).substr(
                                                         offset, chunk_bytes))}}},
                                          until);
                        }
                        reply(message.id, Flag::Terminal, "fs.read.done",
                              {{"size", content.size()}});
                    } else {
                        error(message.id, "unsupported", "unsupported operation: " + message.type);
                    }
                } catch (const ipc::IoError &) {
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
                if (message.type == "exec.cancel" && message.id != 0 && message.id == last_exec_) {
                    if (task_ && !task_->finished) {
                        task_->cancel = true;
                    }
                    return; // A late cancellation never targets a subsequent request.
                }
                require(upload_ && message.id == upload_->id, "event does not belong to an upload");
                const bool exec = upload_->kind == Upload::Kind::Stdin;
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
                        task_->stdin_data += data;
                    } else {
                        upload_->writer->append(data);
                    }
                    upload_->received += data.size();
                    return;
                }
                require(message.type == end_type && upload_->received == upload_->expected,
                        "incomplete or invalid upload termination");
                if (exec) {
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
                    execute_task(*task);
                });
            }

            void execute_task(Task &task) {
                Result result;
                std::string failure;
                bool connection_failed = false;
                try {
                    reply(task.id, Flag::Event, "exec.started", Json::object());
                    size_t stdout_offset = 0, stderr_offset = 0;
                    auto options = task.options;
                    options.stdin_data = task.stdin_data;
                    options.on_output = [&](bool stderr_stream, std::string_view bytes) {
                        auto &offset = stderr_stream ? stderr_offset : stdout_offset;
                        reply(task.id, Flag::Event, stderr_stream ? "exec.stderr" : "exec.stdout",
                              {{"offset", offset}, {"data", binary_bytes(bytes)}});
                        offset += bytes.size();
                    };
                    auto argv = task.argv;
                    if (!config_.task_launcher.empty()) {
                        argv.insert(argv.begin(),
                                    {config_.task_launcher.string(), "--run-task", "--"});
                    }
                    result = lib::run_process(argv, options);
                } catch (const IoError &) {
                    connection_failed = true;
                } catch (const std::exception &error) {
                    failure = error.what();
                }

                // Reclaim descendants once, including when execution or streaming failed.
                try {
                    if (config_.cleanup_tasks) {
                        config_.cleanup_tasks();
                    }
                } catch (...) {
                    task.finished = true;
                    session_.invalidate();
                    return;
                }
                // Execution and cleanup are complete before the terminal becomes visible.
                // The receive loop joins this worker before accepting the next command.
                task.finished = true;
                if (connection_failed) {
                    session_.invalidate();
                    return;
                }
                try {
                    if (!failure.empty()) {
                        error(task.id, "exec_error", failure);
                    } else {
                        reply(task.id, Flag::Terminal, "exec.exited",
                              {{"code", uint32_t(result.runtime_status)},
                               {"timed_out", result.timed_out},
                               {"output_limited", result.output_limited},
                               {"cancelled", result.cancelled}});
                    }
                } catch (...) {
                    session_.invalidate();
                }
            }

            // Declared before task_: the session outlives cancellation/join during unwind.
            ipc::Session session_;
            ServiceConfig config_;
            WorkspaceFiles files_;
            Limits limits_;
            std::optional<Upload> upload_;
            std::unique_ptr<Task> task_;
            uint32_t last_request_ = 0;
            uint32_t last_exec_ = 0;
        };
    } // namespace

    void serve(lib::UniqueFd descriptor, const ServiceConfig &config) {
        Service(std::move(descriptor), config).run();
    }
} // namespace agentd
