#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>

std::expected<std::filesystem::path, std::string> ExecutableDirectory();
// The system's folder picker: the shell dialog on Windows, zenity or else kdialog on Linux. nullopt when cancelled or no picker is installed.
std::optional<std::filesystem::path> PickFolder(const char* Title);
