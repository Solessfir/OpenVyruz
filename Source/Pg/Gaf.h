#pragma once

#include <array>
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

constexpr uint32_t Tag(const char (&Text)[5])
{
    return uint32_t(uint8_t(Text[0])) | uint32_t(uint8_t(Text[1])) << 8 | uint32_t(uint8_t(Text[2])) << 16 | uint32_t(uint8_t(Text[3])) << 24;
}

inline constexpr size_t SlotCount = 4;
using FSlots = std::array<std::vector<uint8_t>, SlotCount>;

// Decoded slot streams of the first DATA entry with the given tag (GafParseSlots at 0047bf00). A missing entry yields empty slots.
std::optional<FSlots> ReadEntrySlots(std::span<const uint8_t> File, uint32_t EntryTag);

// Parses back-to-back BMP_ chunks, as stored in GOB_ slot 1 and BLK_ slot streams.
std::optional<std::vector<FFrame>> ParseBitmaps(std::span<const uint8_t> Bitmaps);
}
