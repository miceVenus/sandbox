#pragma once

#include "lib/process.hpp"
#include "lib/string.hpp"
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unistd.h>

namespace test {
    namespace fs = std::filesystem;

    inline void check(bool value, const std::string &message) {
        if (!value) {
            throw std::runtime_error(message);
        }
    }

    template <class F> std::string rejects(F operation) {
        try {
            operation();
        } catch (const std::exception &error) {
            return error.what();
        }
        throw std::runtime_error("expected rejection");
    }

    struct TemporaryDirectory {
        fs::path path;
        TemporaryDirectory() {
            char pattern[] = "/tmp/sandbox SDK tests-XXXXXX";
            const auto created = mkdtemp(pattern);
            check(created != nullptr, "cannot create temporary directory");
            path = created;
        }
        ~TemporaryDirectory() {
            std::error_code error;
            fs::remove_all(path, error);
        }
        TemporaryDirectory(const TemporaryDirectory &) = delete;
        TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;
    };

    inline void write(const fs::path &path, std::string_view content) {
        std::ofstream out(path, std::ios::binary);
        out.write(content.data(), content.size());
        check(bool(out), "cannot write fixture: " + path.string());
    }

    inline std::string read(const fs::path &path) {
        std::ifstream in(path, std::ios::binary);
        check(bool(in), "cannot read fixture: " + path.string());
        return {(std::istreambuf_iterator<char>(in)), {}};
    }

    // Git CLI belongs to the test fixture and the simulated Agent, not the SDK backend.
    inline std::string git(const fs::path &repo, std::vector<std::string> arguments,
                           std::string_view input = {}) {
        arguments.insert(arguments.begin(), {"/usr/bin/git", "-C", repo.string()});
        lib::ProcessOptions options;
        options.output_limit = 2 * 1024 * 1024;
        options.stdin_data = input;
        const auto result = lib::run_process(arguments, options);
        check(result.runtime_status == 0 && !result.timed_out && !result.output_limited,
              "fixture Git failed: " + result.err);
        return result.out;
    }

    inline void commit(const fs::path &repo, const std::string &message) {
        git(repo, {"add", "."});
        git(repo, {"-c", "user.name=Test", "-c", "user.email=test@example.invalid", "commit",
                   "--allow-empty", "-qm", message});
    }
} // namespace test
