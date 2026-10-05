#include "lib.hpp"

#include <fstream>
#include <iomanip>
#include <sstream>

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

std::string new_id() {
    unsigned char bytes[16];
    std::ifstream random("/dev/urandom", std::ios::binary);
    random.read(reinterpret_cast<char *>(bytes), sizeof bytes);
    require(bool(random), "cannot generate ID");
    std::ostringstream out;
    for (auto b : bytes) {
        out << std::hex << std::setw(2) << std::setfill('0') << unsigned(b);
    }
    return out.str();
}

void relative_path(const fs::path &path) {
    require(!path.empty() && !path.is_absolute(), "cwd must be a relative path");
    for (const auto &part : path) {
        require(part != "..", "cwd traversal rejected");
    }
    require(path.string().find('\0') == std::string::npos, "NUL in path");
}

fs::path get_cwd(const fs::path &root, const fs::path &relative) {
    relative_path(relative);
    return (root / relative).lexically_normal();
}

std::string trim(std::string s) {
    while (!s.empty() && s.back() == '\n') {
        s.pop_back();
    }
    return s;
}
