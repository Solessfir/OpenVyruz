#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

// Indeo Video 5, which the original left to Intel's codec (Ir50_32.dll) through MCI.
// Only the intra pictures of single-band, untiled streams that Intro.avi uses are decoded (Docs/Intro.md).
namespace Indeo5
{
// The stream's planes in its own order.
enum class EPlane : uint8_t
{
    Luma,
    V,
    U,
    Count,
};

// A plane's samples around zero, in a buffer rounded up to whole macroblocks.
struct FPlane
{
    uint32_t Width = 0;
    uint32_t Height = 0;
    uint32_t Pitch = 0;
    uint32_t Rows = 0;
    uint32_t BlockSize = 0;
    uint32_t MacroblockSize = 0;
    std::vector<int16_t> Samples;
};

class FDecoder
{
public:
    // Decodes the next frame; an empty frame repeats the one before.
    std::expected<void, std::string> Decode(std::span<const uint8_t> Frame);

    uint32_t GetWidth() const { return Planes[size_t(EPlane::Luma)].Width; }
    uint32_t GetHeight() const { return Planes[size_t(EPlane::Luma)].Height; }
    // A sample with the bias added back, clamped to a byte.
    uint8_t GetSample(EPlane Plane, uint32_t X, uint32_t Y) const;
    // The picture as R, G, B and A bytes, Width by Height, each chroma sample covering 4x4 pixels.
    void ConvertToRgba(std::span<uint32_t> Pixels) const;

private:
    std::array<FPlane, size_t(EPlane::Count)> Planes;
    bool bDecoded = false;
};
}
