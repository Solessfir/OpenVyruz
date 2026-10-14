#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// A RIFF AVI file with one video stream and at most one sound stream, which the original played through MCI's avivideo device (Docs/Intro.md).
namespace Avi
{
constexpr uint32_t FourCc(const char (&Text)[5])
{
    return uint32_t(uint8_t(Text[0])) | uint32_t(uint8_t(Text[1])) << 8 | uint32_t(uint8_t(Text[2])) << 16 | uint32_t(uint8_t(Text[3])) << 24;
}

struct FMovie
{
    uint32_t FrameMicroseconds = 0;
    uint32_t Width = 0;
    uint32_t Height = 0;
    uint32_t VideoCompression = 0;
    // Each video frame's bytes in the file, in time order; an empty one repeats the frame before.
    std::vector<std::span<const uint8_t>> Frames;
    // The WAVEFORMATEX tag of the sound stream, zero without one, and its chunks joined.
    uint16_t SoundFormat = 0;
    std::vector<uint8_t> Sound;
};

// File must outlive the result, whose frames point into it.
std::optional<FMovie> Read(std::span<const uint8_t> File);
}
