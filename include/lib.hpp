#pragma once

#include "virtualization/process.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

/*          general check            */
void require(bool ok, const std::string &error);
void checked(const Result &r);
void valid_id(const std::string &id);
void system_error(const char *operation);

/*          string operation            */
auto trim(std::string s) -> std::string;
auto new_id() -> std::string;

/*          filesystem check            */
auto valid_hash(const std::string &value) -> bool;
void require_directory(const fs::path &path);
auto get_cwd(const fs::path &root, const fs::path &relative) -> fs::path;
void relative_path(const fs::path &path);