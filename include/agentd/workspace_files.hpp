#pragma once

#include "lib/descriptor.hpp"
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace agentd {
    // Workspace file access used by the container and Guest agentd service.
    class WorkspaceFiles {
      public:
        explicit WorkspaceFiles(const std::filesystem::path &root, bool mapped_identity = false);
        WorkspaceFiles(const WorkspaceFiles &) = delete;
        auto operator=(const WorkspaceFiles &) -> WorkspaceFiles & = delete;
        [[nodiscard]] auto read(const std::filesystem::path &absolute_path, size_t limit) const
            -> std::string;

        class Write {
          public:
            ~Write();
            Write(const Write &) = delete;
            auto operator=(const Write &) -> Write & = delete;
            void append(std::string_view bytes);
            void commit();

          private:
            friend class WorkspaceFiles;
            explicit Write(bool mapped_identity);
            lib::UniqueFd parent_;
            lib::UniqueFd file_;
            std::string target_;
            std::string temporary_;
            size_t limit_ = 0;
            size_t written_ = 0;
            mode_t mode_ = 0644;
            bool committed_ = false;
            bool mapped_identity_;
        };
        [[nodiscard]] auto begin_write(const std::filesystem::path &absolute_path, size_t limit) const
            -> std::unique_ptr<Write>;

      private:
        [[nodiscard]] auto relative_file(const std::filesystem::path &path) const
            -> std::filesystem::path;
        std::filesystem::path root_;
        lib::UniqueFd root_fd_;
        bool mapped_identity_;
    };
} // namespace agentd
