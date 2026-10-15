#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The archives' 8-bit texts: Windows-1252 in the English build, Windows-1251 in the Russian one, from 0x80 up.
// The codes either leaves undefined keep their C1 control points, so a text converts back byte for byte.
using FCodePage = std::array<char16_t, 128>;

const FCodePage& Cp1252();
const FCodePage& Cp1251();
// The code page a manifest names ("windows-1251" or "windows-1252").
const FCodePage& CodePageNamed(std::string_view Name);

char32_t CodePoint(uint8_t Byte, const FCodePage& Page);
std::string ToUtf8(std::span<const uint8_t> Bytes, const FCodePage& Page);
// The text back in the code page; nullopt when it holds a character the page lacks or is not valid UTF-8.
std::optional<std::vector<uint8_t>> FromUtf8(std::string_view Text, const FCodePage& Page);

inline std::span<const uint8_t> AsBytes(const std::string_view Text)
{
    return { reinterpret_cast<const uint8_t*>(Text.data()), Text.size() };
}

// FNV-1a, which the manifest keeps per written file to tell an edit.
uint64_t HashBytes(std::span<const uint8_t> Bytes);
