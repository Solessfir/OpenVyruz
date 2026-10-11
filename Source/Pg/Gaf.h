#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// Original pg-engine GAF container (pgga9902.cpp / pggob.cpp). Format notes live in README.md.
namespace Gaf
{
// Decodes the pg LZSS variant (LzssDecode at 0049ace0). Returns nullopt unless exactly OutputSize bytes are produced.
std::optional<std::vector<uint8_t>> LzssDecode(std::span<const uint8_t> Input, size_t OutputSize);

struct FFrame
{
    uint32_t Width = 0;
    uint32_t Height = 0;
    // Offset of the trimmed bitmap inside the untrimmed sprite cell (slot 0 metadata).
    int32_t TrimX = 0;
    int32_t TrimY = 0;
    uint16_t ColorKey = 0;
    // RGB555, tightly packed rows; ColorKey marks transparent pixels.
    std::vector<uint16_t> Pixels;
};

// Decodes every sprite frame of the GOB_ entry in a .GAF file.
std::optional<std::vector<FFrame>> LoadFrames(std::span<const uint8_t> File);
}
