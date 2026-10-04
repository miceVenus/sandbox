#include "workspace/git/git_repository_ops.hpp"

#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>

int main(int argc, char **argv) {
    try {
        if (argc < 3) {
            throw std::runtime_error("invalid SDK Git request");
        }
        const std::string operation = argv[1];
        if (operation == "inspect-source" && argc == 4) {
            const auto snapshot = git_storage::worker::inspect_source(argv[2], argv[3]);
            const nlohmann::json record = {
                {"repository", snapshot.repository.string()},
                {"baseline", snapshot.baseline},
                {"head", snapshot.head},
                {"branch",
                 snapshot.branch ? nlohmann::json(*snapshot.branch) : nlohmann::json(nullptr)}};
            std::cout << record.dump();
        } else if (operation == "initialize" && argc == 5) {
            const git_storage::SourceSnapshot snapshot{argv[2], argv[3], {}, std::nullopt};
            git_storage::worker::initialize_snapshot(snapshot, argv[4]);
        } else if (operation == "status" && argc == 3) {
            std::cout << git_storage::worker::status(argv[2]);
        } else if (operation == "diff" && argc == 4) {
            std::cout << git_storage::worker::diff(argv[2], argv[3]);
        } else {
            throw std::runtime_error("invalid SDK Git request");
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 125;
    }
}
