#pragma once

#include <expected>
#include <filesystem>
#include <string>

std::expected<std::filesystem::path, std::string> ExecutableDirectory();
