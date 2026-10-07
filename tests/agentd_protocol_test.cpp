#include "ipc/transport.hpp"
#include "test_support.hpp"
#include "virtualization/agentd_client.hpp"

#include <future>
#include <iostream>
#include <sys/socket.h>
#include <thread>

using test::check;
using test::rejects;

namespace {
    class ScriptedTransport final : public ipc::Transport {
      public:
        explicit ScriptedTransport(std::string input) : input_(std::move(input)) {
        }
        void write_all(std::string_view, ipc::Deadline) override {
        }
        auto read_exact(size_t size, ipc::Deadline) -> std::string override {
            check(size <= input_.size(), "unexpected scripted read");
            auto output = input_.substr(0, size);
            input_.erase(0, size);
            return output;
        }

      private:
        std::string input_;
    };

    ipc::Message ready() {
        const ipc::Limits limits;
        return {0,
                ipc::Flag::Terminal,
                "core.ready",
                {{"protocol", ipc::protocol_name},
                 {"version", ipc::protocol_version},
                 {"file_bytes", limits.file_bytes},
                 {"stdin_bytes", limits.stdin_bytes},
                 {"output_bytes", limits.output_bytes},
                 {"timeout_ms", limits.timeout_ms},
                 {"capabilities", {"exec", "exec.cancel", "fs.read", "fs.write"}}}};
    }
    auto unhex(std::string_view value) -> std::string {
        std::string result;
        for (size_t i = 0; i < value.size(); i += 2) {
            result.push_back(
                static_cast<char>(std::stoul(std::string(value.substr(i, 2)), nullptr, 16)));
        }
        return result;
    }
    auto frame(std::string body) -> std::string {
        const uint32_t length = body.size() + 5;
        std::string result(9, '\0');
        for (int i = 0; i < 4; ++i) {
            result[i] = static_cast<char>(length >> (24 - 8 * i));
        }
        result += body;
        return result;
    }
    auto body_with_payload(std::string payload) -> std::string {
        const ipc::Json envelope{
            {"v", ipc::protocol_version}, {"t", "test"}, {"p", ipc::binary_bytes(payload)}};
        const auto data = ipc::Json::to_cbor(envelope);
        return {data.begin(), data.end()};
    }
} // namespace

auto main() -> int {
    try {
        const ipc::Message message{
            0x01020304, ipc::Flag::Terminal, "exec.exited", {{"code", uint32_t(7)}}};
        const auto golden =
            unhex("000000210102030402a3617047a164636f64650761746b657865632e657869746564617601");
        check(ipc::encode(message) == golden, "wire bytes changed");
        const auto decoded = ipc::decode(golden);
        check(decoded.id == message.id && decoded.type == message.type &&
                  decoded.payload == message.payload,
              "golden frame did not decode");
        const std::string binary("\0\xff\x80", 3);
        check(ipc::binary_string(ipc::binary_bytes(binary), 3) == binary, "binary bytes changed");
        rejects([&] {
            ipc::decode(golden + "x");
        });
        auto bad_flag = golden;
        bad_flag[8] = 3;
        rejects([&] {
            ipc::decode(bad_flag);
        });
        // Duplicate keys, trailing data, indefinite maps, tags, floats, excessive nesting.
        for (const auto &payload : {unhex("a2616101616102"), unhex("a000"), unhex("bfff"),
                                    unhex("c0a0"), unhex("a16161f93c00"), unhex("a161ff01"),
                                    unhex("a1616162c080"), std::string(26, '\x81') + "\xa0"}) {
            rejects([&] {
                ipc::decode(frame(body_with_payload(payload)));
            });
        }
        rejects([&] {
            ipc::decode(frame(unhex("a2617601617601")));
        });
        auto wrong_version =
            ipc::Json{{"v", uint32_t(2)}, {"t", "test"}, {"p", ipc::binary_bytes("\xa0")}};
        const auto encoded = ipc::Json::to_cbor(wrong_version);
        rejects([&] {
            ipc::decode(frame({encoded.begin(), encoded.end()}));
        });
        // Unknown optional payload fields remain readable.
        const auto extra =
            ipc::decode(ipc::encode({1, ipc::Flag::Request, "test", {{"future", true}}}));
        check(extra.payload.at("future") == true, "optional field lost");

        int sockets[2];
        check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0,
              "socketpair failed");
        ipc::Channel receiver(ipc::adopt_descriptor(sockets[0], ipc::DescriptorKind::Socket));
        auto writer = std::async(std::launch::async, [&] {
            // Deliberately fragment the header and body into single-byte writes.
            for (char byte : golden) {
                check(send(sockets[1], &byte, 1, MSG_NOSIGNAL) == 1, "fragment write failed");
            }
        });
        check(receiver.receive(ipc::Clock::now() + std::chrono::seconds(1)).id == message.id,
              "fragmented frame failed");
        writer.get();
        const char huge[] = {0x7f, -1, -1, -1};
        check(send(sockets[1], huge, 4, MSG_NOSIGNAL) == 4, "header write failed");
        rejects([&] {
            receiver.receive(ipc::Clock::now() + std::chrono::seconds(1));
        });
        rejects([&] {
            receiver.receive(ipc::Clock::now() + std::chrono::seconds(1));
        });
        close(sockets[1]);
        // The same framing works with character-device style read/write operations.
        check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0,
              "socketpair failed");
        {
            ipc::Channel sender(ipc::adopt_descriptor(sockets[0], ipc::DescriptorKind::SerialPort));
            ipc::Channel recipient(
                ipc::adopt_descriptor(sockets[1], ipc::DescriptorKind::SerialPort));
            const auto until = ipc::Clock::now() + std::chrono::seconds(1);
            sender.send(message, until);
            check(recipient.receive(until).id == message.id, "character I/O adapter failed");
        }
        // A compromised Guest must not bypass Host offset, data-type and ID checks.
        for (const auto &bad :
             {ipc::Message{1,
                           ipc::Flag::Event,
                           "fs.read.data",
                           {{"offset", size_t(1)}, {"data", ipc::binary_bytes("x")}}},
              ipc::Message{
                  1, ipc::Flag::Event, "fs.read.data", {{"offset", size_t(0)}, {"data", "text"}}},
              ipc::Message{2, ipc::Flag::Terminal, "fs.read.done", {{"size", size_t(0)}}}}) {
            auto script = ipc::encode(ready()) + ipc::encode(bad);
            virtualization::AgentdClient client(
                std::make_unique<ScriptedTransport>(std::move(script)));
            client.handshake();
            rejects([&] {
                client.read("/workspace/a", 100);
            });
            check(rejects([&] {
                      client.ping();
                  }).find("unusable") != std::string::npos,
                  "malformed Guest response did not invalidate connection");
        }

        check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0,
              "socketpair failed");
        ipc::Channel stalled(ipc::adopt_descriptor(sockets[0], ipc::DescriptorKind::Socket));
        check(send(sockets[1], golden.data(), 6, MSG_NOSIGNAL) == 6, "partial header write failed");
        const auto started = ipc::Clock::now();
        rejects([&] {
            stalled.receive(started + std::chrono::milliseconds(30));
        });
        check(ipc::Clock::now() - started < std::chrono::seconds(1), "transport timeout blocked");
        close(sockets[1]);
        std::cout << "agentd protocol passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
