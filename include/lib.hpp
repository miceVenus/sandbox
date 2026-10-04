#pragma once

#include "../process.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

void require(bool ok, const std::string &error);
void checked(const Result &r);
void valid_id(const std::string &id);
std::string trim(std::string s);

std::string new_id();
fs::path get_cwd(const fs::path &root, const fs::path &relative);

void relative_path(const fs::path &path);
