#include "lib/json.hpp"
#include "lib/error.hpp"
#include <fstream>

namespace lib {
    void write_json(const std::filesystem::path &path, const nlohmann::json &value) {
        std::ofstream output(path);
        output << value.dump(2) << '\n';
        output.close();
        require(static_cast<bool>(output), "cannot write JSON: " + path.string());
    }
} // namespace lib
