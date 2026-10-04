#include "../include/agent_client.hpp"
#include "../include/communication/agent_transport.hpp"
#include "test_support.hpp"

#include <future>
#include <iostream>
#include <sys/socket.h>
#include <thread>

namespace agent = protocol;
using test::check;
using test::rejects;

namespace {
    class ScriptedTransport final : public agent::Transport {
      public:
        explicit ScriptedTransport(std::string input) : input_(std::move(input)) {
        }
        void write_all(std::string_view, agent::Deadline) override {
        }
        std::string read_exact(size_t size, agent::Deadline) override {
            check(size <= input_.size(), "unexpected scripted read");
            auto output = input_.substr(0, size);
            input_.erase(0, size);
            return output;
        }

      private:
        std::string input_;
    };

    agent::Message ready() {
        const agent::Limits limits;
        return {0,
                agent::Flag::Terminal,
                "core.ready",
                {{"protocol", agent::protocol_name},
                 {"version", agent::protocol_version},
                 {"file_bytes", limits.file_bytes},
                 {"stdin_bytes", limits.stdin_bytes},
                 {"output_bytes", limits.output_bytes},
                 {"timeout_ms", limits.timeout_ms},
                 {"capabilities", {"exec", "exec.cancel", "fs.read", "fs.write"}}}};
    }
    std::string unhex(std::string_view value) {
        std::string result;
        for (size_t i = 0; i < value.size(); i += 2) {
            result.push_back(
                static_cast<char>(std::stoul(std::string(value.substr(i, 2)), nullptr, 16)));
        }
        return result;
    }
    std::string frame(std::string body) {
        const uint32_t length = body.size() + 5;
        std::string result(9, '\0');
        for (int i = 0; i < 4; ++i) {
            result[i] = static_cast<char>(length >> (24 - 8 * i));
        }
        result += body;
        return result;
    }
    std::string body_with_payload(std::string payload) {
        const agent::Json envelope{
            {"v", agent::protocol_version}, {"t", "test"}, {"p", agent::binary_bytes(payload)}};
        const auto data = agent::Json::to_cbor(envelope);
        return {data.begin(), data.end()};
    }
} // namespace

int main() {
    try {
        const agent::Message message{
            0x01020304, agent::Flag::Terminal, "exec.exited", {{"code", uint32_t(7)}}};
        const auto golden =
            unhex("000000210102030402a3617047a164636f64650761746b657865632e657869746564617601");
        check(agent::encode(message) == golden, "wire bytes changed");
        const auto decoded = agent::decode(golden);
        check(decoded.id == message.id && decoded.type == message.type &&
                  decoded.payload == message.payload,
              "golden frame did not decode");
        const std::string binary("\0\xff\x80", 3);
        check(agent::binary_string(agent::binary_bytes(binary), 3) == binary,
              "binary bytes changed");
        rejects([&] { agent::decode(golden + "x"); });
        auto bad_flag = golden;
        bad_flag[8] = 3;
        rejects([&] { agent::decode(bad_flag); });
        // Duplicate keys, trailing data, indefinite maps, tags, floats, excessive nesting.
        for (const auto &payload : {unhex("a2616101616102"),
                                    unhex("a000"),
                                    unhex("bfff"),
                                    unhex("c0a0"),
                                    unhex("a16161f93c00"),
                                    unhex("a161ff01"),
                                    unhex("a1616162c080"),
                                    std::string(26, '\x81') + "\xa0"}) {
            rejects([&] { agent::decode(frame(body_with_payload(payload))); });
        }
        rejects([&] { agent::decode(frame(unhex("a2617601617601"))); });
        auto wrong_version =
            agent::Json{{"v", uint32_t(2)}, {"t", "test"}, {"p", agent::binary_bytes("\xa0")}};
        const auto encoded = agent::Json::to_cbor(wrong_version);
        rejects([&] { agent::decode(frame({encoded.begin(), encoded.end()})); });
        // Unknown optional payload fields remain readable.
        const auto extra =
            agent::decode(agent::encode({1, agent::Flag::Request, "test", {{"future", true}}}));
        check(extra.payload.at("future") == true, "optional field lost");

        int sockets[2];
        check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0,
              "socketpair failed");
        agent::Channel receiver(agent::adopt_descriptor(sockets[0], agent::DescriptorKind::Socket));
        auto writer = std::async(std::launch::async, [&] {
            // Deliberately fragment the header and body into single-byte writes.
            for (char byte : golden) {
                check(send(sockets[1], &byte, 1, MSG_NOSIGNAL) == 1, "fragment write failed");
            }
        });
        check(receiver.receive(agent::Clock::now() + std::chrono::seconds(1)).id == message.id,
              "fragmented frame failed");
        writer.get();
        const char huge[] = {0x7f, -1, -1, -1};
        check(send(sockets[1], huge, 4, MSG_NOSIGNAL) == 4, "header write failed");
        rejects([&] { receiver.receive(agent::Clock::now() + std::chrono::seconds(1)); });
        rejects([&] { receiver.receive(agent::Clock::now() + std::chrono::seconds(1)); });
        close(sockets[1]);
        // The same framing works with character-device style read/write operations.
        check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0,
              "socketpair failed");
        {
            agent::Channel sender(
                agent::adopt_descriptor(sockets[0], agent::DescriptorKind::SerialPort));
            agent::Channel recipient(
                agent::adopt_descriptor(sockets[1], agent::DescriptorKind::SerialPort));
            const auto until = agent::Clock::now() + std::chrono::seconds(1);
            sender.send(message, until);
            check(recipient.receive(until).id == message.id, "character I/O adapter failed");
        }
        // A compromised Guest must not bypass Host offset, data-type and ID checks.
        for (const auto &bad :
             {agent::Message{1,
                             agent::Flag::Event,
                             "fs.read.data",
                             {{"offset", size_t(1)}, {"data", agent::binary_bytes("x")}}},
              agent::Message{
                  1, agent::Flag::Event, "fs.read.data", {{"offset", size_t(0)}, {"data", "text"}}},
              agent::Message{2, agent::Flag::Terminal, "fs.read.done", {{"size", size_t(0)}}}}) {
            auto script = agent::encode(ready()) + agent::encode(bad);
            agent::Client client(std::make_unique<ScriptedTransport>(std::move(script)));
            client.handshake();
            rejects([&] { client.read("/workspace/a", 100); });
            check(rejects([&] { client.ping(); }).find("unusable") != std::string::npos,
                  "malformed Guest response did not invalidate connection");
        }

        check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0,
              "socketpair failed");
        agent::Channel stalled(agent::adopt_descriptor(sockets[0], agent::DescriptorKind::Socket));
        check(send(sockets[1], golden.data(), 6, MSG_NOSIGNAL) == 6, "partial header write failed");
        const auto started = agent::Clock::now();
        rejects([&] { stalled.receive(started + std::chrono::milliseconds(30)); });
        check(agent::Clock::now() - started < std::chrono::seconds(1), "transport timeout blocked");
        close(sockets[1]);
        std::cout << "agent protocol passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
