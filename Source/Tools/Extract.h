#pragma once

#include <filesystem>
#include <optional>

// --extract: the original archives as a tree of modern files (Docs/Extract.md). Input is the game folder (vz-data.rsc, and Vz-slide.rsc into Slides) or one .rsc file;
// Output defaults to an Extracted folder beside it. Returns the process exit code: non-zero when any entry failed.
int RunExtract(const std::filesystem::path& Input, const std::optional<std::filesystem::path>& Output);
