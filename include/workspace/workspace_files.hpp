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
    WorkspaceFiles &operator=(const WorkspaceFiles &) = delete;
    std::string read(const std::filesystem::path &absolute_path, size_t limit) const;

    class Write {
      public:
        ~Write();
        Write(const Write &) = delete;
        Write &operator=(const Write &) = delete;
        void append(std::string_view bytes);
        void commit();

      private:
        friend class WorkspaceFiles;
        struct State;
        explicit Write(std::unique_ptr<State> state);
        std::unique_ptr<State> state_;
    };
    std::unique_ptr<Write> begin_write(const std::filesystem::path &absolute_path,
                                       size_t limit) const;

  private:
    std::filesystem::path relative_file(const std::filesystem::path &path) const;
    std::filesystem::path root_;
    int root_fd_;
};
