#pragma once

#include <filesystem>
#include <optional>

// --compress: an --extract tree back into archives the original and the port read (Docs/Extract.md). Entries whose files are unchanged copy the source archive's bytes;
// edited ones convert back where a converter exists, and any other edit fails the run rather than write a broken archive. Output defaults to a Packed folder beside the tree.
// Returns the process exit code.
int RunCompress(const std::filesystem::path& Extracted, const std::optional<std::filesystem::path>& Output);

// --verify-extract: extracts the game folder's archives, packs them back unedited, and checks every entry reads back byte for byte; then edits a text and an image and checks
// both come back through their converters. Returns the process exit code.
int VerifyExtractRoundTrip(const std::filesystem::path& GameFolder);
