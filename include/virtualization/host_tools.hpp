#pragma once

#include <filesystem>
#include <vector>

struct ToolMount {
    std::filesystem::path source;
    std::filesystem::path destination;
};

// Administrator-selected system directories. No caller-supplied host mounts.
std::vector<ToolMount> host_tool_mounts();
void validate_host_tools(const std::filesystem::path &source_repository,
                         const std::filesystem::path &manager_root);
void prepare_host_tools(const std::filesystem::path &rootfs);
