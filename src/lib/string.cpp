#include "lib/string.hpp"
#include "lib/error.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace lib {
    auto trim_newlines(std::string value) -> std::string {
        while (!value.empty() && value.back() == '\n') {
            value.pop_back();
        }
        return value;
    }

    auto random_id() -> std::string {
        std::array<unsigned char, 16> bytes{};
        std::ifstream random("/dev/urandom", std::ios::binary);
        random.read(reinterpret_cast<char *>(bytes.data()), sizeof(bytes));
        require(static_cast<bool>(random), "cannot generate random ID");
        std::ostringstream output;
        for (const auto byte : bytes) {
            output << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
        }
        return output.str();
    }

    auto is_hex(std::string_view value) -> bool {
        return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
        });
    }
} // namespace lib
