#include "Pg/Tga.h"

#include "Pg/Binary.h"

#include <algorithm>
#include <vector>

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
constexpr uint8_t CompressedTrueColor = 10;
constexpr uint8_t Rgb555Bits = 16;
constexpr uint8_t RgbBits = 24;
constexpr uint8_t RgbaBits = 32;
constexpr size_t BitsPerByte = 8;
constexpr uint8_t TopToBottomFlag = 0x20;
constexpr uint32_t MaxDimension = 4096;
// A run-length packet's header: the top bit repeats one pixel, otherwise raw pixels follow; the low bits hold the count less one.
constexpr uint8_t RunPacketFlag = 0x80;
constexpr uint8_t PacketCountMask = 0x7f;

// One stored pixel to RGB555: 16-bit pixels are already A1R5G5B5, the others blue, green and red bytes.
uint16_t ToRgb555(const uint8_t* const Pixel, const size_t BytesPerPixel)
{
    constexpr uint16_t Rgb555Mask = 0x7fff;
    constexpr int ChannelShift = 3;
    constexpr int RedShift = 10;
    constexpr int GreenShift = 5;
    if (BytesPerPixel == Rgb555Bits / BitsPerByte)
    {
        return uint16_t((Pixel[0] | Pixel[1] << BitsPerByte) & Rgb555Mask);
    }

    return uint16_t((Pixel[2] >> ChannelShift) << RedShift | (Pixel[1] >> ChannelShift) << GreenShift | Pixel[0] >> ChannelShift);
}
}

std::optional<Gaf::FFrame> LoadTga(const std::span<const uint8_t> File)
{
    const std::optional<FTgaHeader> Header = ReadStruct<FTgaHeader>(File, 0);
    if (!Header || (Header->ImageType != UncompressedTrueColor && Header->ImageType != CompressedTrueColor) || Header->ColorMapType != 0 ||
        (Header->BitsPerPixel != Rgb555Bits && Header->BitsPerPixel != RgbBits && Header->BitsPerPixel != RgbaBits) || Header->Width == 0 ||
        Header->Height == 0 || Header->Width > MaxDimension || Header->Height > MaxDimension)
    {
        return std::nullopt;
    }

    const size_t BytesPerPixel = Header->BitsPerPixel / BitsPerByte;
    const size_t PixelCount = size_t{ Header->Width } * Header->Height;
    size_t Offset = sizeof(FTgaHeader) + Header->IdLength;
    if (File.size() < Offset)
    {
        return std::nullopt;
    }

    // The pixels in file order, uncompressed or as run-length packets.
    std::vector<uint16_t> Stored;
    Stored.reserve(PixelCount);
    while (Stored.size() < PixelCount)
    {
        size_t Count = PixelCount;
        bool bRun = false;
        if (Header->ImageType == CompressedTrueColor)
        {
            if (Offset >= File.size())
            {
                return std::nullopt;
            }

            const uint8_t Packet = File[Offset++];
            bRun = (Packet & RunPacketFlag) != 0;
            Count = std::min(size_t(Packet & PacketCountMask) + 1, PixelCount - Stored.size());
        }

        const size_t Bytes = bRun ? BytesPerPixel : Count * BytesPerPixel;
        if (File.size() - Offset < Bytes)
        {
            return std::nullopt;
        }

        for (size_t Index = 0; Index < Count; ++Index)
        {
            Stored.push_back(ToRgb555(&File[Offset + (bRun ? 0 : Index * BytesPerPixel)], BytesPerPixel));
        }

        Offset += Bytes;
    }

    // Rows run bottom to top unless the descriptor says otherwise.
    Gaf::FFrame Frame{ .Width = Header->Width, .Height = Header->Height };
    Frame.Pixels.resize(PixelCount);
    const bool bTopDown = (Header->Descriptor & TopToBottomFlag) != 0;
    for (uint32_t Row = 0; Row < Header->Height; ++Row)
    {
        const uint32_t Target = bTopDown ? Row : Header->Height - 1 - Row;
        std::copy_n(Stored.begin() + ptrdiff_t(size_t{ Row } * Header->Width), Header->Width, Frame.Pixels.begin() + ptrdiff_t(size_t{ Target } * Header->Width));
    }

    // A key outside RGB555 keeps every pixel opaque.
    constexpr uint16_t NoColorKey = 0xffff;
    Frame.ColorKey = NoColorKey;
    return Frame;
}
