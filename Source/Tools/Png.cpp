#include "Tools/Png.h"

#include <algorithm>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>

namespace
{
constexpr size_t RgbaChannels = 4;
constexpr uint8_t Opaque = 0xff;
}

FImage ImageFromFrame(const Gaf::FFrame& Frame)
{
    constexpr int RedShift = 10;
    constexpr int GreenShift = 5;
    constexpr uint16_t ChannelMask = 0x1f;
    constexpr int WidenShift = 3;
    constexpr int FillShift = 2;
    const auto Widen = [](const uint16_t Channel) { return uint8_t(Channel << WidenShift | Channel >> FillShift); };
    FImage Image{ .Width = Frame.Width, .Height = Frame.Height };
    Image.Pixels.resize(size_t(Frame.Width) * Frame.Height * RgbaChannels);
    for (size_t Index = 0; Index < Frame.Pixels.size() && Index * RgbaChannels < Image.Pixels.size(); ++Index)
    {
        const uint16_t Pixel = Frame.Pixels[Index];
        uint8_t* const Out = &Image.Pixels[Index * RgbaChannels];
        if (Pixel == Frame.ColorKey)
        {
            std::fill_n(Out, RgbaChannels, uint8_t(0));
            continue;
        }

        Out[0] = Widen(Pixel >> RedShift & ChannelMask);
        Out[1] = Widen(Pixel >> GreenShift & ChannelMask);
        Out[2] = Widen(Pixel & ChannelMask);
        Out[3] = Opaque;
    }

    return Image;
}

void Blit(FImage& Target, const FImage& Source, const uint32_t X, const uint32_t Y, const bool bMirrorX, const bool bMirrorY)
{
    for (uint32_t Row = 0; Row < Source.Height && Y + Row < Target.Height; ++Row)
    {
        const uint32_t SourceRow = bMirrorY ? Source.Height - 1 - Row : Row;
        for (uint32_t Column = 0; Column < Source.Width && X + Column < Target.Width; ++Column)
        {
            const uint32_t SourceColumn = bMirrorX ? Source.Width - 1 - Column : Column;
            std::copy_n(&Source.Pixels[(size_t(SourceRow) * Source.Width + SourceColumn) * RgbaChannels], RgbaChannels,
                &Target.Pixels[(size_t(Y + Row) * Target.Width + X + Column) * RgbaChannels]);
        }
    }
}

FPlace FramePlace(const Gaf::FAnimationFrame& Frame, const Gaf::FFrame& Bitmap)
{
    return {
        .X = Frame.bMirrorX ? Frame.OffsetX - Bitmap.TrimX - int32_t(Bitmap.Width) : Frame.OffsetX + Bitmap.TrimX,
        .Y = Frame.bMirrorY ? Frame.OffsetY - Bitmap.TrimY - int32_t(Bitmap.Height) : Frame.OffsetY + Bitmap.TrimY,
    };
}

std::optional<std::vector<uint8_t>> EncodePng(const FImage& Image)
{
    std::vector<uint8_t> Bytes;
    const auto Append = [](void* const Context, void* const Data, const int Size)
    {
        auto& Out = *static_cast<std::vector<uint8_t>*>(Context);
        const auto* const First = static_cast<const uint8_t*>(Data);
        Out.insert(Out.end(), First, First + Size);
    };
    if (Image.Width == 0 || Image.Height == 0 ||
        stbi_write_png_to_func(Append, &Bytes, int(Image.Width), int(Image.Height), int(RgbaChannels), Image.Pixels.data(), int(Image.Width * RgbaChannels)) == 0)
    {
        return std::nullopt;
    }

    return Bytes;
}
