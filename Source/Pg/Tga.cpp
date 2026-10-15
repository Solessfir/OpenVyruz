#include "Pg/Tga.h"

#include "Pg/Binary.h"

#include <algorithm>
#include <cstring>
#include <string_view>
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

std::optional<std::vector<uint8_t>> EncodeTga(const FTgaImage& Image, const std::span<const uint8_t> Original)
{
    const std::optional<FTgaHeader> Source = ReadStruct<FTgaHeader>(Original, 0);
    if (!Source || Image.Width == 0 || Image.Height == 0 || Image.Width > MaxDimension || Image.Height > MaxDimension ||
        Image.Pixels.size() != size_t{ Image.Width } * Image.Height * RgbaChannels || Original.size() < sizeof(FTgaHeader) + Source->IdLength)
    {
        return std::nullopt;
    }

    // The original's type, depth, row order and ID field stay; a 32-bit or unknown depth comes back as 24-bit.
    FTgaHeader Header = *Source;
    Header.ImageType = Source->ImageType == CompressedTrueColor ? CompressedTrueColor : UncompressedTrueColor;
    Header.ColorMapType = 0;
    std::fill(std::begin(Header.ColorMapSpec), std::end(Header.ColorMapSpec), uint8_t(0));
    Header.BitsPerPixel = Source->BitsPerPixel == Rgb555Bits ? Rgb555Bits : RgbBits;
    Header.Width = uint16_t(Image.Width);
    Header.Height = uint16_t(Image.Height);
    const size_t BytesPerPixel = Header.BitsPerPixel / BitsPerByte;
    const size_t Start = sizeof(FTgaHeader) + Source->IdLength;
    // A 16-bit file's top bit is an attribute the game ignores; the original's first pixel says how it was set.
    constexpr uint16_t AttributeBit = 0x8000;
    const uint16_t Attribute = BytesPerPixel == Rgb555Bits / BitsPerByte && Original.size() >= Start + BytesPerPixel ? uint16_t(Original[Start + 1] << BitsPerByte) & AttributeBit : 0;
    constexpr int NarrowShift = 3;

    std::vector<uint8_t> Out(sizeof(FTgaHeader));
    std::memcpy(Out.data(), &Header, sizeof(Header));
    Out.insert(Out.end(), Original.begin() + sizeof(FTgaHeader), Original.begin() + ptrdiff_t(Start));
    const bool bTopDown = (Header.Descriptor & TopToBottomFlag) != 0;
    std::vector<uint8_t> Stored;
    Stored.reserve(size_t{ Image.Width } * Image.Height * BytesPerPixel);
    for (uint32_t Row = 0; Row < Image.Height; ++Row)
    {
        const uint32_t SourceRow = bTopDown ? Row : Image.Height - 1 - Row;
        for (uint32_t Column = 0; Column < Image.Width; ++Column)
        {
            const uint8_t* const Pixel = &Image.Pixels[(size_t(SourceRow) * Image.Width + Column) * RgbaChannels];
            if (BytesPerPixel == Rgb555Bits / BitsPerByte)
            {
                const uint16_t Value = uint16_t(Attribute | (Pixel[0] >> NarrowShift) << RedShift | (Pixel[1] >> NarrowShift) << GreenShift | Pixel[2] >> NarrowShift);
                Stored.push_back(uint8_t(Value));
                Stored.push_back(uint8_t(Value >> BitsPerByte));
            }
            else
            {
                Stored.push_back(Pixel[2]);
                Stored.push_back(Pixel[1]);
                Stored.push_back(Pixel[0]);
            }
        }
    }

    // A TGA 2.0 footer with no extension or developer area to point at is kept as the original had it.
    constexpr std::string_view Signature = "TRUEVISION-XFILE.";
    constexpr size_t FooterSize = 26;
    constexpr size_t FooterOffsets = 8;
    const std::span<const uint8_t> Footer = Original.size() >= Start + FooterSize ? Original.last(FooterSize) : std::span<const uint8_t>{};
    const bool bKeepFooter = !Footer.empty() && std::equal(Signature.begin(), Signature.end(), Footer.begin() + FooterOffsets) &&
                             std::all_of(Footer.begin(), Footer.begin() + FooterOffsets, [](const uint8_t Byte) { return Byte == 0; });
    const auto AppendFooter = [&]
    {
        if (bKeepFooter)
        {
            Out.insert(Out.end(), Footer.begin(), Footer.end());
        }
    };
    if (Header.ImageType == UncompressedTrueColor)
    {
        Out.insert(Out.end(), Stored.begin(), Stored.end());
        AppendFooter();
        return Out;
    }

    // Run-length packets of at most 128 pixels, never crossing a row as the format asks: a run where a pixel repeats, raw pixels otherwise.
    constexpr size_t MaxPacket = PacketCountMask + 1;
    const size_t RowPixels = Image.Width;
    const auto Same = [&](const size_t A, const size_t B) { return std::equal(&Stored[A * BytesPerPixel], &Stored[(A + 1) * BytesPerPixel], &Stored[B * BytesPerPixel]); };
    for (size_t RowStart = 0; RowStart < Stored.size() / BytesPerPixel; RowStart += RowPixels)
    {
        const size_t RowEnd = RowStart + RowPixels;
        for (size_t Pixel = RowStart; Pixel < RowEnd;)
        {
            size_t Run = 1;
            while (Pixel + Run < RowEnd && Run < MaxPacket && Same(Pixel, Pixel + Run))
            {
                ++Run;
            }

            if (Run > 1)
            {
                Out.push_back(uint8_t(RunPacketFlag | (Run - 1)));
                Out.insert(Out.end(), &Stored[Pixel * BytesPerPixel], &Stored[(Pixel + 1) * BytesPerPixel]);
                Pixel += Run;
                continue;
            }

            size_t Raw = 1;
            while (Pixel + Raw < RowEnd && Raw < MaxPacket && !(Pixel + Raw + 1 < RowEnd && Same(Pixel + Raw, Pixel + Raw + 1)))
            {
                ++Raw;
            }

            Out.push_back(uint8_t(Raw - 1));
            Out.insert(Out.end(), &Stored[Pixel * BytesPerPixel], &Stored[(Pixel + Raw) * BytesPerPixel]);
            Pixel += Raw;
        }
    }

    AppendFooter();
    return Out;
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
