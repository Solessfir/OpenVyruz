#include "Pg/Tga.h"

#include "Pg/Binary.h"

namespace
{
#pragma pack(push, 1)
struct FTgaHeader
{
    uint8_t IdLength;
    uint8_t ColorMapType;
    uint8_t ImageType;
    uint8_t ColorMapSpec[5];
    uint16_t OriginX;
    uint16_t OriginY;
    uint16_t Width;
    uint16_t Height;
    uint8_t BitsPerPixel;
    uint8_t Descriptor;
};
#pragma pack(pop)
static_assert(sizeof(FTgaHeader) == 18);

constexpr uint8_t UncompressedTrueColor = 2;
constexpr uint8_t RgbBits = 24;
constexpr uint8_t RgbaBits = 32;
constexpr size_t BitsPerByte = 8;
constexpr uint8_t TopToBottomFlag = 0x20;
constexpr uint32_t MaxDimension = 4096;
}

std::optional<Gaf::FFrame> LoadTga(const std::span<const uint8_t> File)
{
    const std::optional<FTgaHeader> Header = ReadStruct<FTgaHeader>(File, 0);
    if (!Header || Header->ImageType != UncompressedTrueColor || Header->ColorMapType != 0 || (Header->BitsPerPixel != RgbBits && Header->BitsPerPixel != RgbaBits) ||
        Header->Width == 0 || Header->Height == 0 || Header->Width > MaxDimension || Header->Height > MaxDimension)
    {
        return std::nullopt;
    }

    const size_t BytesPerPixel = Header->BitsPerPixel / BitsPerByte;
    const size_t PixelStart = sizeof(FTgaHeader) + Header->IdLength;
    const size_t PixelCount = size_t{ Header->Width } * Header->Height;
    if (File.size() < PixelStart || (File.size() - PixelStart) / BytesPerPixel < PixelCount)
    {
        return std::nullopt;
    }

    // Pixels are stored blue, green, red; rows run bottom to top unless the descriptor says otherwise.
    constexpr int ChannelShift = 3;
    constexpr int RedShift = 10;
    constexpr int GreenShift = 5;
    Gaf::FFrame Frame{ .Width = Header->Width, .Height = Header->Height };
    Frame.Pixels.resize(PixelCount);
    const bool bTopDown = (Header->Descriptor & TopToBottomFlag) != 0;
    for (uint32_t Row = 0; Row < Header->Height; ++Row)
    {
        const uint32_t Target = bTopDown ? Row : Header->Height - 1 - Row;
        for (uint32_t Column = 0; Column < Header->Width; ++Column)
        {
            const uint8_t* const Pixel = &File[PixelStart + (size_t{ Row } * Header->Width + Column) * BytesPerPixel];
            Frame.Pixels[size_t{ Target } * Header->Width + Column] =
                uint16_t((Pixel[2] >> ChannelShift) << RedShift | (Pixel[1] >> ChannelShift) << GreenShift | Pixel[0] >> ChannelShift);
        }
    }

    // A key outside RGB555 keeps every pixel opaque.
    constexpr uint16_t NoColorKey = 0xffff;
    Frame.ColorKey = NoColorKey;
    return Frame;
}
