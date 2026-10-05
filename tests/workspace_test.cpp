#include "../include/workspace/workspace.hpp"
#include "test_support.hpp"
#include <iostream>
#include <random>
#include <sys/stat.h>

namespace fs = std::filesystem;
using test::check;
using test::git;
using test::read;
using test::rejects;
using test::trim;
using test::write;

int main() {
    try {
        test::TemporaryDirectory temp;
        const auto repo = temp.path / "source repo";
        fs::create_directory(repo);
        git(repo, {"init", "-q"});
        test::commit(repo, "parent");
        const auto parent = trim(git(repo, {"rev-parse", "HEAD"}));
        write(repo / "main.txt", "baseline\n");
        write(repo / "remove.txt", "delete me\n");
        write(repo / ".gitignore", "cache/\n");
        test::commit(repo, "baseline");
        const auto baseline = trim(git(repo, {"rev-parse", "HEAD"}));
        const auto info = temp.path / "info one";
        const auto workspace = GitWorkspace::create(repo, info);
        const auto files = workspace.files_path();
        check(workspace.baseline() == baseline && read(files / "main.txt") == "baseline\n",
              "wrong snapshot");
        check(fs::is_directory(files / ".git"), "B is not a repository");
        check(trim(git(files, {"rev-parse", "HEAD"})) == baseline &&
                  trim(git(files, {"rev-parse", "HEAD^"})) == parent,
              "history was lost");
        check(trim(git(files, {"branch", "--show-current"})) == "agent" &&
                  git(files, {"remote"}).empty() && !fs::exists(files / ".git/FETCH_HEAD"),
              "unexpected task Git configuration");
        check(workspace.status().empty() && workspace.diff().empty(), "snapshot is dirty");

        fs::create_directory(repo / "nested");
        const auto nested = GitWorkspace::create(repo / "nested", temp.path / "nested info");
        check(nested.source_repository() == repo, "subdirectory source not resolved");
        const auto linked = temp.path / "linked worktree";
        git(repo, {"worktree", "add", "--detach", linked.string(), baseline});
        fs::create_directory(linked / "nested");
        GitWorkspace::create(linked / "nested", temp.path / "linked info");

        write(files / "main.txt", "agent edit\n");
        fs::remove(files / "remove.txt");
        write(files / "new.txt", "new file\n");
        const std::string binary("\0\xff\0", 3);
        write(files / "binary.bin", binary);
        fs::create_directory(files / "cache");
        write(files / "cache/ignored.txt", "ignored\n");
        const auto status = workspace.status();
        for (const auto name : {"main.txt", "new.txt", "remove.txt"}) {
            check(status.find(name) != std::string::npos, "status omitted a change");
        }
        const auto patch = workspace.diff();
        for (const auto text :
             {"+agent edit", "new file mode", "deleted file mode", "GIT binary patch"}) {
            check(patch.find(text) != std::string::npos, "patch omitted a change");
        }
        check(patch.find("ignored.txt") == std::string::npos, "ignored file was exported");
        const auto receiver = temp.path / "patch receiver";
        git(temp.path, {"clone", "--quiet", repo.string(), receiver.string()});
        git(receiver, {"apply", "--binary", "-"}, patch);
        check(read(receiver / "binary.bin") == binary && !fs::exists(receiver / "remove.txt"),
              "binary patch is not applicable");
        check(read(repo / "main.txt") == "baseline\n" &&
                  git(repo, {"status", "--porcelain"}).empty() &&
                  trim(git(repo, {"rev-parse", "HEAD"})) == baseline,
              "A was modified");

        write(files / "main.txt", "baseline\n");
        write(files / "remove.txt", "delete me\n");
        fs::remove(files / "new.txt");
        fs::remove(files / "binary.bin");
        check(workspace.diff().empty(), "private index retained stale changes");
        rejects([&] { GitWorkspace::create(repo, info); });
        rejects([&] { GitWorkspace::create(repo, repo / "nested info"); });
        check(!fs::exists(repo / "nested info"), "failed create left a directory");
        write(repo / "main.txt", "host uncommitted\n");
        rejects([&] { GitWorkspace::create(repo, temp.path / "dirty info"); });
        check(!fs::exists(temp.path / "dirty info"), "dirty create left a directory");
        git(repo, {"restore", "main.txt"});

        check(fs::is_directory(info / "manager.git"), "private baseline missing");
        fs::create_directory(files / "nested");
        write(files / "nested/.git", "gitdir: /some/host/path\n");
        rejects([&] { workspace.diff(); });
        fs::remove_all(files / "nested");
        check(mkfifo((files / "fifo").c_str(), 0600) == 0, "FIFO fixture failed");
        rejects([&] { workspace.status(); });
        fs::remove(files / "fifo");
        fs::create_hard_link(files / "main.txt", files / "hardlink.txt");
        rejects([&] { workspace.diff(); });
        fs::remove(files / "hardlink.txt");
        fs::create_symlink("main.txt", files / "link.txt");
        check(workspace.diff().find("120000") != std::string::npos, "symlink mode lost");
        fs::remove(files / "link.txt");

        write(files / "main.txt", "committed in B\n");
        test::commit(files, "agent change");
        check(trim(git(files, {"rev-parse", "HEAD"})) != baseline, "agent commit failed");
        git(files, {"config", "core.worktree", repo.string()});
        git(files, {"config", "core.hooksPath", (temp.path / "agent-hooks").string()});
        check(workspace.diff().find("+committed in B") != std::string::npos &&
                  read(repo / "main.txt") == "baseline\n",
              "B config affected inspection");

        std::mt19937 random(42);
        std::string huge(1024 * 1024, '\0');
        for (auto &byte : huge) {
            byte = static_cast<char>(random());
        }
        write(files / "huge.bin", huge);
        check(rejects([&] { workspace.diff(); }).find("exceeded 1 MiB") != std::string::npos,
              "patch output was not bounded");
        fs::remove(files / "huge.bin");
        check(workspace.diff().find("+committed in B") != std::string::npos,
              "preview did not recover");
        rejects([&] { GitWorkspace::create(repo, temp.path / "bad revision", "not-a-revision"); });
        write(info / "session.txt", "broken\n");
        check(rejects([&] { GitWorkspace::open(info).status(); }).find("invalid session metadata") !=
                  std::string::npos,
              "corrupt workspace metadata was not rejected");
        std::cout << "SDK Git workspace tests passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
