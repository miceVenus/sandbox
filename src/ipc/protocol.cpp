#include "ipc/protocol.hpp"

#include <set>

namespace ipc {
    namespace {
        void append_u32(std::string &output, uint32_t value) {
            for (int shift = 24; shift >= 0; shift -= 8) {
                output.push_back(static_cast<char>(value >> shift));
            }
        }

        auto read_u32(std::string_view input, size_t offset) -> uint32_t {
            uint32_t value = 0;
            for (size_t i = 0; i < 4; ++i) {
                value = (value << 8) | static_cast<uint8_t>(input[offset + i]);
            }
            return value;
        }

        // Validate before constructing a JSON DOM. Definite containers only;
        // bounded depth/nodes and unique text keys prevent ambiguous or huge trees.
        class CborValidator {
          public:
            explicit CborValidator(std::string_view input) : input_(input) {
            }

            void validate() {
                item(0);
                require(offset_ == input_.size(), "trailing CBOR input");
            }

          private:
            auto byte() -> uint8_t {
                require(offset_ < input_.size(), "truncated CBOR input");
                return static_cast<uint8_t>(input_[offset_++]);
            }

            auto argument(uint8_t info) -> uint64_t {
                if (info < 24) {
                    return info;
                }
                require(info <= 27, "indefinite or reserved CBOR length");
                uint64_t value = 0;
                for (size_t i = 0; i < (size_t(1) << (info - 24)); ++i) {
                    value = (value << 8) | byte();
                }
                return value;
            }

            auto string(uint8_t head) -> std::string_view {
                const auto length = argument(head & 31);
                require(length <= input_.size() - offset_, "truncated CBOR string");
                const auto result = input_.substr(offset_, static_cast<size_t>(length));
                offset_ += static_cast<size_t>(length);
                if ((head >> 5) == 3) {
                    validate_utf8(result);
                }
                return result;
            }

            void validate_utf8(std::string_view text) {
                size_t i = 0;
                while (i < text.size()) {
                    const auto first = static_cast<uint8_t>(text[i++]);
                    if (first < 0x80) {
                        continue;
                    }
                    const size_t trailing = first >= 0xc2 && first <= 0xdf   ? 1
                                            : first >= 0xe0 && first <= 0xef ? 2
                                            : first >= 0xf0 && first <= 0xf4 ? 3
                                                                             : 0;
                    require(trailing > 0 && trailing <= text.size() - i, "invalid UTF-8 CBOR text");
                    uint32_t codepoint = first & (0x7f >> (trailing + 1));
                    for (size_t j = 0; j < trailing; ++j) {
                        const auto next = static_cast<uint8_t>(text[i++]);
                        require((next & 0xc0) == 0x80, "invalid UTF-8 continuation");
                        codepoint = (codepoint << 6) | (next & 0x3f);
                    }
                    const uint32_t minimum = trailing == 1 ? 0x80 : trailing == 2 ? 0x800 : 0x10000;
                    require(codepoint >= minimum && codepoint <= 0x10ffff &&
                                !(codepoint >= 0xd800 && codepoint <= 0xdfff),
                            "invalid UTF-8 codepoint");
                }
            }

            void item(size_t depth) {
                require(depth <= 24 && ++nodes_ <= 8192, "CBOR complexity limit exceeded");
                const auto head = byte();
                const auto major = head >> 5;
                if (major <= 1) {
                    argument(head & 31);
                } else if (major == 2 || major == 3) {
                    string(head);
                } else if (major == 4 || major == 5) {
                    const auto count = argument(head & 31);
                    require(count <= 8192, "CBOR container limit exceeded");
                    std::set<std::string_view> keys;
                    for (uint64_t i = 0; i < count; ++i) {
                        if (major == 5) {
                            require(++nodes_ <= 8192, "CBOR complexity limit exceeded");
                            const auto key_head = byte();
                            require((key_head >> 5) == 3, "CBOR map keys must be text");
                            require(keys.insert(string(key_head)).second, "duplicate CBOR key");
                        }
                        item(depth + 1);
                    }
                } else {
                    require(head == 0xf4 || head == 0xf5 || head == 0xf6,
                            "CBOR tags, floats and other simple values are unsupported");
                }
            }

            std::string_view input_;
            size_t offset_ = 0;
            size_t nodes_ = 0;
        };

        auto checked_cbor(std::string_view bytes) -> Json {
            CborValidator(bytes).validate();
            try {
                return Json::from_cbor(bytes.begin(), bytes.end(), true, true);
            } catch (const Json::exception &error) {
                throw ProtocolError(std::string("invalid CBOR: ") + error.what());
            }
        }
    } // namespace

    void require(bool condition, const std::string &message) {
        if (!condition) {
            throw ProtocolError(message);
        }
    }

    auto unsigned_field(const Json &value, const char *key, uint64_t maximum) -> uint64_t {
        require(value.is_object() && value.contains(key) && value.at(key).is_number_unsigned(),
                std::string("expected unsigned field: ") + key);
        const auto result = value.at(key).get<uint64_t>();
        require(result <= maximum, std::string("field exceeds limit: ") + key);
        return result;
    }

    auto text_field(const Json &value, const char *key, size_t maximum) -> std::string {
        require(value.is_object() && value.contains(key) && value.at(key).is_string(),
                std::string("expected text field: ") + key);
        const auto result = value.at(key).get<std::string>();
        require(result.size() <= maximum && result.find('\0') == std::string::npos,
                std::string("invalid text field: ") + key);
        return result;
    }

    auto binary_bytes(std::string_view content) -> Json {
        return Json::binary(std::vector<uint8_t>(content.begin(), content.end()));
    }

    auto binary_string(const Json &value, size_t limit) -> std::string {
        require(value.is_binary(), "expected CBOR byte string");
        const auto &data = value.get_binary();
        require(data.size() <= limit, "byte string exceeds limit");
        return {data.begin(), data.end()};
    }

    auto encode(const Message &message) -> std::string {
        require(!message.type.empty() && message.type.size() <= 64 &&
                    message.type.find('\0') == std::string::npos && message.payload.is_object(),
                "invalid outgoing message");
        const auto payload = Json::to_cbor(message.payload);
        const Json envelope{
            {"v", protocol_version}, {"t", message.type}, {"p", Json::binary(payload)}};
        const auto body = Json::to_cbor(envelope);
        require(body.size() + 5 <= max_frame_bytes, "frame exceeds limit");
        std::string frame;
        append_u32(frame, static_cast<uint32_t>(body.size() + 5));
        append_u32(frame, message.id);
        frame.push_back(static_cast<char>(message.flag));
        frame.append(reinterpret_cast<const char *>(body.data()), body.size());
        // Local callers must satisfy the same contract as peers.
        decode(frame);
        return frame;
    }

    auto decode(std::string_view frame) -> Message {
        require(frame.size() >= 9, "frame too short");
        const auto length = read_u32(frame, 0);
        require(length >= 5 && length <= max_frame_bytes && frame.size() == length + size_t(4),
                "invalid frame length");
        const auto flag = static_cast<uint8_t>(frame[8]);
        require(flag <= static_cast<uint8_t>(Flag::Terminal), "invalid frame flag");
        const auto envelope = checked_cbor(frame.substr(9));
        require(unsigned_field(envelope, "v", UINT32_MAX) == protocol_version,
                "unsupported protocol version");
        const auto type = text_field(envelope, "t", 64);
        require(!type.empty() && envelope.contains("p"), "invalid message envelope");
        const auto payload_bytes = binary_string(envelope.at("p"), max_frame_bytes);
        auto payload = checked_cbor(payload_bytes);
        require(payload.is_object(), "payload must be a map");
        return {read_u32(frame, 4), static_cast<Flag>(flag), type, std::move(payload)};
    }
} // namespace ipc
