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
constexpr size_t RgbaChannels = 4;
constexpr uint8_t TopToBottomFlag = 0x20;
constexpr uint32_t MaxDimension = 4096;
// A run-length packet's header: the top bit repeats one pixel, otherwise raw pixels follow; the low bits hold the count less one.
constexpr uint8_t RunPacketFlag = 0x80;
constexpr uint8_t PacketCountMask = 0x7f;
constexpr int RedShift = 10;
constexpr int GreenShift = 5;
constexpr uint16_t ChannelMask = 0x1f;
constexpr int WidenShift = 3;
constexpr int FillShift = 2;
constexpr uint8_t Opaque = 0xff;

// One stored pixel to RGBA: 16-bit pixels are A1R5G5B5, widened so that narrowing by >> 3 gives them back; the others blue, green and red bytes. Every pixel is opaque, as the game draws them.
void ToRgba(const uint8_t* const Pixel, const size_t BytesPerPixel, uint8_t* const Out)
{
    if (BytesPerPixel == Rgb555Bits / BitsPerByte)
    {
        const uint16_t Value = uint16_t(Pixel[0] | Pixel[1] << BitsPerByte);
        const auto Widen = [](const uint16_t Channel) { return uint8_t(Channel << WidenShift | Channel >> FillShift); };
        Out[0] = Widen(Value >> RedShift & ChannelMask);
        Out[1] = Widen(Value >> GreenShift & ChannelMask);
        Out[2] = Widen(Value & ChannelMask);
    }
    else
    {
        Out[0] = Pixel[2];
        Out[1] = Pixel[1];
        Out[2] = Pixel[0];
    }

    Out[3] = Opaque;
}
}

std::optional<FTgaImage> DecodeTga(const std::span<const uint8_t> File)
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
    std::vector<uint8_t> Stored;
    Stored.reserve(PixelCount * RgbaChannels);
    while (Stored.size() < PixelCount * RgbaChannels)
    {
        const size_t Done = Stored.size() / RgbaChannels;
        size_t Count = PixelCount - Done;
        bool bRun = false;
        if (Header->ImageType == CompressedTrueColor)
        {
            if (Offset >= File.size())
            {
                return std::nullopt;
            }

            const uint8_t Packet = File[Offset++];
            bRun = (Packet & RunPacketFlag) != 0;
            Count = std::min(size_t(Packet & PacketCountMask) + 1, PixelCount - Done);
        }

        const size_t Bytes = bRun ? BytesPerPixel : Count * BytesPerPixel;
        if (File.size() - Offset < Bytes)
        {
            return std::nullopt;
        }

        for (size_t Index = 0; Index < Count; ++Index)
        {
            Stored.resize(Stored.size() + RgbaChannels);
            ToRgba(&File[Offset + (bRun ? 0 : Index * BytesPerPixel)], BytesPerPixel, &Stored[Stored.size() - RgbaChannels]);
        }

        Offset += Bytes;
    }

    // Rows run bottom to top unless the descriptor says otherwise.
    FTgaImage Image{ .Width = Header->Width, .Height = Header->Height };
    Image.Pixels.resize(Stored.size());
    const bool bTopDown = (Header->Descriptor & TopToBottomFlag) != 0;
    const size_t RowBytes = size_t{ Header->Width } * RgbaChannels;
    for (uint32_t Row = 0; Row < Header->Height; ++Row)
    {
        const uint32_t Target = bTopDown ? Row : Header->Height - 1 - Row;
        std::copy_n(Stored.begin() + ptrdiff_t(Row * RowBytes), RowBytes, Image.Pixels.begin() + ptrdiff_t(Target * RowBytes));
    }

    return Image;
}

std::optional<Gaf::FFrame> LoadTga(const std::span<const uint8_t> File)
{
    const std::optional<FTgaImage> Image = DecodeTga(File);
    if (!Image)
    {
        return std::nullopt;
    }

    Gaf::FFrame Frame{ .Width = Image->Width, .Height = Image->Height };
    Frame.Pixels.resize(size_t(Image->Width) * Image->Height);
    constexpr int NarrowShift = 3;
    for (size_t Index = 0; Index < Frame.Pixels.size(); ++Index)
    {
        const uint8_t* const Pixel = &Image->Pixels[Index * RgbaChannels];
        Frame.Pixels[Index] = uint16_t((Pixel[0] >> NarrowShift) << RedShift | (Pixel[1] >> NarrowShift) << GreenShift | Pixel[2] >> NarrowShift);
    }

    // A key outside RGB555 keeps every pixel opaque.
    constexpr uint16_t NoColorKey = 0xffff;
    Frame.ColorKey = NoColorKey;
    return Frame;
}
