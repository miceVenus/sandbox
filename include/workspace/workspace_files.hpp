#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

// Runtime-local file access shared by the container and Guest task service.
class WorkspaceFiles {
  public:
    explicit WorkspaceFiles(const std::filesystem::path &root);
    ~WorkspaceFiles();
    WorkspaceFiles(const WorkspaceFiles &) = delete;
    auto operator=(const WorkspaceFiles &) -> WorkspaceFiles & = delete;
    [[nodiscard]] auto read(const std::filesystem::path &absolute_path, size_t limit) const -> std::string;

    class Write {
      public:
        ~Write();
        Write(const Write &) = delete;
        auto operator=(const Write &) -> Write & = delete;
        void append(std::string_view bytes);
        void commit();

      private:
        friend class WorkspaceFiles;
        struct State;
        explicit Write(std::unique_ptr<State> state);
        std::unique_ptr<State> state_;
    };
    [[nodiscard]] auto begin_write(const std::filesystem::path &absolute_path,
                                       size_t limit) const -> std::unique_ptr<Write>;

  private:
    [[nodiscard]] auto relative_file(const std::filesystem::path &path) const -> std::filesystem::path;
    std::filesystem::path root_;
    int root_fd_;
};
