#include "lib.hpp"

#include <algorithm>
#include <fstream>
#include <array>
#include <stdexcept>
#include <cstring>
#include <iomanip>
#include <sstream>

/*          general check            */

void require(bool ok, const std::string &error) {
    if (!ok) {
        throw std::runtime_error(error);
    }
}

void checked(const Result &r) {
    require(r.runtime_status == 0 && !r.timed_out && !r.output_limited, "crun failed: " + r.err);
}

void valid_id(const std::string &id) {
    require(id.size() == 32 && id.find_first_not_of("0123456789abcdef") == std::string::npos,
            "invalid sandbox ID");
}

void system_error(const char *operation) {
    throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
}


/*          string operation            */

auto trim(std::string s) -> std::string {
    while (!s.empty() && s.back() == '\n') {
        s.pop_back();
    }
    return s;
}

auto new_id() -> std::string {
    std::array<unsigned char, 16> bytes;
    std::ifstream random("/dev/urandom", std::ios::binary);
    random.read(reinterpret_cast<char *>(bytes.data()), sizeof bytes);
    require(static_cast<bool>(random), "cannot generate ID");
    std::ostringstream out;
    for (auto b : bytes) {
        out << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(b);
    }
    return out.str();
}

/*          filesystem check            */

auto valid_hash(const std::string &value) -> bool {
    return (value.size() == 40 || value.size() == 64) &&
            std::all_of(value.begin(), value.end(), [](char c) -> bool {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            });
}

void require_directory(const fs::path &path) {
    if (fs::symlink_status(path).type() != fs::file_type::directory) {
        throw std::runtime_error("expected a real directory: " + path.string());
    }
}

void relative_path(const fs::path &path) {
    require(!path.empty() && !path.is_absolute(), "cwd must be a relative path");
    for (const auto &part : path) {
        require(part != "..", "cwd traversal rejected");
    }
    require(path.string().find('\0') == std::string::npos, "NUL in path");
}

auto get_cwd(const fs::path &root, const fs::path &relative) -> fs::path {
    relative_path(relative);
    return (root / relative).lexically_normal();
}
