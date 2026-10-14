#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

std::expected<std::filesystem::path, std::string> ExecutableDirectory();
// The system's folder picker: the shell dialog on Windows, zenity or else kdialog on Linux. nullopt when cancelled or no picker is installed.
std::optional<std::filesystem::path> PickFolder(const char* Title);
// FUN_00404850 before every file the original writes: an existing file becomes <name>.bak, replacing the last one.
void BackUpFile(const std::filesystem::path& Path);
// Writes the whole file after backing up the old one; false on failure.
bool WriteFileText(const std::filesystem::path& Path, std::string_view Text);
