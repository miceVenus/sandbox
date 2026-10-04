#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

namespace protocol {
    using Json = nlohmann::json;
    inline constexpr uint32_t protocol_version = 1;
    inline constexpr const char *protocol_name = "bbm.sandbox.agent";
    inline constexpr size_t max_frame_bytes = 64 * 1024;
    inline constexpr size_t chunk_bytes = 16 * 1024;

    struct Limits {
        size_t file_bytes = 8 * 1024 * 1024;
        size_t stdin_bytes = 8 * 1024 * 1024;
        size_t output_bytes = 8 * 1024 * 1024;
        uint32_t timeout_ms = 60000;
    };

    enum class Flag : uint8_t { Event = 0, Request = 1, Terminal = 2 };

    struct ProtocolError : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    // Fixed wire header: [length:u32 BE][id:u32 BE][flag:u8].
    // The envelope is CBOR {v:uint, t:text, p:bytes}; p is independently encoded CBOR.
    struct Message {
        uint32_t id = 0;
        Flag flag = Flag::Event;
        std::string type;
        Json payload = Json::object();
    };

    std::string encode(const Message &message);
    Message decode(std::string_view complete_frame);
    Json binary_bytes(std::string_view content);
    std::string binary_string(const Json &value, size_t limit);
    uint64_t unsigned_field(const Json &value, const char *key, uint64_t maximum);
    std::string text_field(const Json &value, const char *key, size_t maximum = 4096);
    void require(bool condition, const std::string &message);
} // namespace protocol
