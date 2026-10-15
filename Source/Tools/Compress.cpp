#include "Tools/Compress.h"

#include "Game/Engine.h"
#include "Game/Level.h"
#include "Pg/Binary.h"
#include "Pg/Font.h"
#include "Pg/Gaf.h"
#include "Pg/ResourceArchive.h"
#include "Pg/Sound.h"
#include "Pg/Tga.h"
#include "Tools/Extract.h"
#include "Tools/Json.h"
#include "Tools/Png.h"
#include "Tools/Text.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <expected>
#include <format>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#include <stb_image.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace
{
namespace fs = std::filesystem;

using FBytes = std::vector<uint8_t>;
template <typename T>
using FResult = std::expected<T, std::string>;

std::optional<FBytes> ReadFile(const fs::path& Path)
{
    std::ifstream File(Path, std::ios::binary);
    if (!File)
    {
        return std::nullopt;
    }

    return FBytes(std::istreambuf_iterator<char>(File), std::istreambuf_iterator<char>());
}

bool WriteFile(const fs::path& Path, const std::span<const uint8_t> Bytes)
{
    std::ofstream File(Path, std::ios::binary);
    File.write(reinterpret_cast<const char*>(Bytes.data()), std::streamsize(Bytes.size()));
    return bool(File);
}

std::string_view AsText(const std::span<const uint8_t> Bytes)
{
    return { reinterpret_cast<const char*>(Bytes.data()), Bytes.size() };
}

fs::path PathFromUtf8(const std::string_view Text)
{
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(Text.data()), Text.size()));
}

std::string Lower(std::string Text)
{
    std::ranges::transform(Text, Text.begin(), [](const char Character) { return char(std::tolower(uint8_t(Character))); });
    return Text;
}

std::string Extension(const std::string_view Name)
{
    const size_t Dot = Name.rfind('.');
    return Dot == std::string_view::npos ? std::string() : Lower(std::string(Name.substr(Dot + 1)));
}

int32_t Int(const FJsonValue& Value)
{
    return int32_t(std::llround(Value.AsNumber()));
}

uint32_t Unsigned(const FJsonValue& Value)
{
    return uint32_t(std::llround(Value.AsNumber()));
}

std::optional<FJsonValue> ParseJsonFile(const std::span<const uint8_t> Bytes)
{
    return ParseJson(AsText(Bytes));
}

std::optional<FImage> DecodePng(const std::span<const uint8_t> Bytes)
{
    constexpr int RgbaChannels = 4;
    int Width = 0;
    int Height = 0;
    int Channels = 0;
    stbi_uc* const Pixels = stbi_load_from_memory(Bytes.data(), int(Bytes.size()), &Width, &Height, &Channels, RgbaChannels);
    if (!Pixels)
    {
        return std::nullopt;
    }

    FImage Image{ .Width = uint32_t(Width), .Height = uint32_t(Height) };
    Image.Pixels.assign(Pixels, Pixels + size_t(Width) * size_t(Height) * RgbaChannels);
    stbi_image_free(Pixels);
    return Image;
}

// A rectangle of an image back to RGB555, the inverse of ImageFromFrame: a pixel less than half opaque becomes the colour key.
std::optional<Gaf::FFrame> FrameFromImage(const FImage& Image, const uint32_t X, const uint32_t Y, const uint32_t Width, const uint32_t Height, const uint16_t ColorKey)
{
    constexpr size_t RgbaChannels = 4;
    constexpr uint8_t OpaqueThreshold = 0x80;
    constexpr int NarrowShift = 3;
    constexpr int RedShift = 10;
    constexpr int GreenShift = 5;
    constexpr uint16_t LowestBlue = 1;
    if (X > Image.Width || Width > Image.Width - X || Y > Image.Height || Height > Image.Height - Y)
    {
        return std::nullopt;
    }

    Gaf::FFrame Frame{ .Width = Width, .Height = Height, .ColorKey = ColorKey };
    Frame.Pixels.reserve(size_t(Width) * Height);
    for (uint32_t Row = 0; Row < Height; ++Row)
    {
        for (uint32_t Column = 0; Column < Width; ++Column)
        {
            const uint8_t* const Pixel = &Image.Pixels[(size_t(Y + Row) * Image.Width + X + Column) * RgbaChannels];
            const uint16_t Color = uint16_t((Pixel[0] >> NarrowShift) << RedShift | (Pixel[1] >> NarrowShift) << GreenShift | Pixel[2] >> NarrowShift);
            // An opaque pixel that lands on the key would vanish; the nearest blue keeps it.
            Frame.Pixels.push_back(Pixel[3] < OpaqueThreshold ? ColorKey : Color == ColorKey ? uint16_t(Color ^ LowestBlue) : Color);
        }
    }

    return Frame;
}

// A PNG back to a Targa in the original file's form.
std::optional<FBytes> TgaFromPng(const std::span<const uint8_t> Png, const std::span<const uint8_t> Original)
{
    const std::optional<FImage> Image = DecodePng(Png);
    return Image ? EncodeTga({ .Width = Image->Width, .Height = Image->Height, .Pixels = Image->Pixels }, Original) : std::nullopt;
}

FLevelPropertyBlock BlockFromJson(const FJsonValue& Json)
{
    FLevelPropertyBlock Block{ .Kind = std::string(Json["Kind"].AsString()), .Name = std::string(Json["Name"].AsString()) };
    for (const FJsonValue& Field : Json["Fields"].Elements)
    {
        Block.Fields.emplace_back(Field["Name"].AsString(), Field["Value"].AsString());
    }

    for (const FJsonValue& Child : Json["Children"].Elements)
    {
        Block.Children.push_back(BlockFromJson(Child));
    }

    return Block;
}

// How the original writes each kind of block file, which its parser holds it to: the level files (.3DO, .3DP, .3DX) end lines with a bare LF and crash the
// original with a CR, and give the root's fields as "NAME [value]"; the class libraries use CR LF and "name = value" at the root too.
struct FBlockStyle
{
    std::string_view LineEnd;
    bool bBracketRoot = false;
};
constexpr FBlockStyle LevelStyle{ .LineEnd = "\n", .bBracketRoot = true };
constexpr FBlockStyle ClassStyle{ .LineEnd = "\r\n", .bBracketRoot = false };

// A block in the .3DC/.3DO/.3DP grammar: fields as "Name = value" lines (the root's maybe as "NAME [value]"), tabs for depth.
void AppendBlock(std::string& Out, const FLevelPropertyBlock& Block, const size_t Depth, const FBlockStyle& Style)
{
    const std::string_view LineEnd = Style.LineEnd;
    const std::string Indent(Depth == 0 ? 0 : Depth - 1, '\t');
    if (Depth > 0)
    {
        Out += std::format("{}{}{}{} [{}", Indent, Block.Kind, Block.Name.empty() ? "" : " ", Block.Name, LineEnd);
    }

    for (const auto& [Name, Value] : Block.Fields)
    {
        if (Depth == 0 && Style.bBracketRoot)
        {
            Out += Value.empty() ? std::format("{}{}", Name, LineEnd) : std::format("{} [{}]{}", Name, Value, LineEnd);
        }
        else
        {
            const std::string_view FieldIndent = Depth == 0 ? "" : "\t";
            Out += Value.empty() ? std::format("{}{}{}{}", Indent, FieldIndent, Name, LineEnd) : std::format("{}{}{}\t= {}{}", Indent, FieldIndent, Name, Value, LineEnd);
        }
    }

    for (const FLevelPropertyBlock& Child : Block.Children)
    {
        Out += Depth == 0 ? LineEnd : "";
        AppendBlock(Out, Child, Depth + 1, Style);
    }

    if (Depth > 0)
    {
        Out += std::format("{}]{}", Indent, LineEnd);
    }
}

std::string BlockSource(const FLevelPropertyBlock& Root, const std::string_view Format, const FBlockStyle& Style)
{
    constexpr std::string_view DefaultFormat = "1.3";
    std::string Out = std::format("{}{}", Format.empty() ? DefaultFormat : Format, Style.LineEnd);
    AppendBlock(Out, Root, 0, Style);
    return Out;
}

std::optional<std::string> BlocksText(const std::span<const uint8_t> Json)
{
    const std::optional<FJsonValue> Root = ParseJsonFile(Json);
    if (!Root || Root->Type != FJsonValue::EType::Object)
    {
        return std::nullopt;
    }

    return BlockSource(BlockFromJson(*Root), (*Root)["Format"].AsString(), ClassStyle);
}

bool SameBlocks(const FLevelPropertyBlock& A, const FLevelPropertyBlock& B)
{
    return A.Kind == B.Kind && A.Name == B.Name && A.Fields == B.Fields && std::ranges::equal(A.Children, B.Children, SameBlocks);
}

// A file made from an archive entry: whether it differs from what --extract wrote, and whether to convert it back anyway.
struct FMadeFile
{
    fs::path Path;
    FBytes Bytes;
    bool bEdited = false;
    bool bChanged = false;
};

const FMadeFile* FindMade(const std::vector<FMadeFile>& Files, const std::string_view Suffix)
{
    const auto Found = std::ranges::find_if(Files, [&](const FMadeFile& File) { return Lower(File.Path.filename().string()).ends_with(Lower(std::string(Suffix))); });
    return Found == Files.end() ? nullptr : &*Found;
}

std::vector<Gaf::FAnimation> AnimationsFromJson(const FJsonValue& Document)
{
    std::vector<Gaf::FAnimation> Animations;
    for (const FJsonValue& Json : Document["Animations"].Elements)
    {
        Gaf::FAnimation& Animation = Animations.emplace_back();
        Animation.Name = Json["Name"].AsString();
        Animation.LoopStart = Unsigned(Json["LoopStart"]);
        // The readable switches win over the raw flags they mirror.
        Animation.Flags = Unsigned(Json["Flags"]) & ~(Gaf::AnimationLoops | Gaf::AnimationPingPong | Gaf::AnimationReverse);
        Animation.Flags |= (Json["Loop"].bValue ? Gaf::AnimationLoops : 0) | (Json["PingPong"].bValue ? Gaf::AnimationPingPong : 0) |
                           (Json["Reverse"].bValue ? Gaf::AnimationReverse : 0);
        for (const FJsonValue& Frame : Json["Frames"].Elements)
        {
            Animation.Frames.push_back({
                .BitmapIndex = Unsigned(Frame["Bitmap"]),
                .OffsetX = Int(Frame["OffsetX"]),
                .OffsetY = Int(Frame["OffsetY"]),
                .DrawFlags = Unsigned(Frame["DrawFlags"]),
                .bMirrorX = Frame["MirrorX"].bValue,
                .bMirrorY = Frame["MirrorY"].bValue,
                .EventFlags = Unsigned(Frame["EventFlags"]),
                .EventValue = Unsigned(Frame["EventValue"]),
                .Duration = Int(Frame["Duration"]),
            });
        }
    }

    return Animations;
}

// The JSON's fonts over the GAF's own, which fix how many fonts and glyphs there are.
std::optional<std::vector<Font::FFont>> FontsFromJson(const FJsonValue& Document, std::vector<Font::FFont> Fonts)
{
    const std::vector<FJsonValue>& Json = Document["Fonts"].Elements;
    if (Json.size() != Fonts.size())
    {
        return std::nullopt;
    }

    for (size_t Index = 0; Index < Fonts.size(); ++Index)
    {
        const FJsonValue& Source = Json[Index];
        Font::FFont& Font = Fonts[Index];
        Font.Flags = (Unsigned(Source["Flags"]) & ~Font::FontProportional) | (Source["Proportional"].bValue ? Font::FontProportional : 0);
        Font.LineGap = Int(Source["LineGap"]);
        Font.CharSpacing = Int(Source["CharSpacing"]);
        Font.Advance = Int(Source["Advance"]);
        Font.LineHeight = Int(Source["LineHeight"]);
        Font.Baseline = Int(Source["Baseline"]);
        Font.DrawMode = Unsigned(Source["DrawMode"]);
        Font.Color = { .Alpha = Unsigned(Source["Alpha"]), .Diffuse = Unsigned(Source["Diffuse"]), .Specular = Unsigned(Source["Specular"]) };
        std::ranges::fill(Font.Glyphs, Font::FGlyph{});
        for (const FJsonValue& Glyph : Source["Glyphs"].Elements)
        {
            const uint32_t Byte = Unsigned(Glyph["Byte"]);
            if (Byte == 0 || Byte > Font.Glyphs.size())
            {
                return std::nullopt;
            }

            Font.Glyphs[Byte - 1] = {
                .Base = { .BitmapIndex = Unsigned(Glyph["Bitmap"]), .OffsetX = Int(Glyph["OffsetX"]), .OffsetY = Int(Glyph["OffsetY"]) },
                .Overlay = { .BitmapIndex = Unsigned(Glyph["OverlayBitmap"]), .OffsetX = Int(Glyph["OverlayOffsetX"]), .OffsetY = Int(Glyph["OverlayOffsetY"]) },
            };
        }
    }

    return Fonts;
}

// A bitmap as an edited copy of it shows it, and where that copy is.
struct FBitmapEdit
{
    Gaf::FFrame Bitmap;
    std::string Where;
};

bool SameBitmap(const Gaf::FFrame& A, const Gaf::FFrame& B)
{
    return A.Width == B.Width && A.Height == B.Height && A.TrimX == B.TrimX && A.TrimY == B.TrimY && A.Pixels == B.Pixels;
}

// The bitmaps as the edited sheets and pages show them. A cell still showing its original bitmap leaves it alone; an edited cell is trimmed to its opaque pixels,
// which sets the bitmap's size and trim. Every edited copy of a bitmap must agree.
FResult<std::vector<Gaf::FFrame>> BitmapsFromSheets(const std::span<const uint8_t> Original, const FJsonValue& Document, const std::vector<FMadeFile>& Files)
{
    std::optional<std::vector<Gaf::FFrame>> Bitmaps = Gaf::LoadFrames(Original);
    const std::optional<std::vector<Gaf::FAnimation>> OldAnimations = Bitmaps ? Gaf::LoadAnimations(Original, Bitmaps->size()) : std::nullopt;
    if (!Bitmaps || !OldAnimations)
    {
        return std::unexpected("the original GAF's bitmaps do not load");
    }

    std::map<std::string, FResult<FImage>> Sheets;
    const auto Sheet = [&](const std::string_view Name) -> const FResult<FImage>*
    {
        const auto Made = std::ranges::find_if(Files, [&](const FMadeFile& File) { return File.Path.filename() == PathFromUtf8(Name); });
        if (Name.empty() || Made == Files.end() || !Made->bChanged)
        {
            return nullptr;
        }

        auto Found = Sheets.find(std::string(Name));
        if (Found == Sheets.end())
        {
            std::optional<FImage> Image = DecodePng(Made->Bytes);
            Found = Sheets.emplace(std::string(Name), Image ? FResult<FImage>(std::move(*Image)) : std::unexpected(std::format("{} is not a readable PNG", Name))).first;
        }

        return &Found->second;
    };

    std::vector<std::vector<FBitmapEdit>> Edits(Bitmaps->size());
    const auto Valid = [&](const uint32_t Index) { return Index >= 1 && Index <= Bitmaps->size(); };
    const std::vector<Gaf::FAnimation> NewAnimations = AnimationsFromJson(Document);
    const std::vector<FJsonValue>& AnimationJson = Document["Animations"].Elements;
    const FResult<FImage>* const Image = Sheet(Document["Image"].AsString());
    if (Image && !*Image)
    {
        return std::unexpected(Image->error());
    }

    const uint32_t Columns = Unsigned(Document["Columns"]);
    const uint32_t CellWidth = Unsigned(Document["CellWidth"]);
    const uint32_t CellHeight = Unsigned(Document["CellHeight"]);
    const int32_t PivotX = Int(Document["PivotX"]);
    const int32_t PivotY = Int(Document["PivotY"]);
    for (size_t AnimationIndex = 0; Image && AnimationIndex < AnimationJson.size(); ++AnimationIndex)
    {
        const FJsonValue& Json = AnimationJson[AnimationIndex];
        const std::vector<Gaf::FAnimationFrame>& Frames = NewAnimations[AnimationIndex].Frames;
        // A FirstCell of -1 is an animation that draws nothing, so it has no cells.
        if (Int(Json["FirstCell"]) < 0)
        {
            if (std::ranges::any_of(Frames, [&](const Gaf::FAnimationFrame& Frame) { return Valid(Frame.BitmapIndex); }))
            {
                return std::unexpected(std::format("animation {} shows bitmaps but has no FirstCell on the sheet", AnimationIndex + 1));
            }

            continue;
        }

        const uint32_t FirstCell = Unsigned(Json["FirstCell"]);
        for (size_t FrameIndex = 0; FrameIndex < Frames.size(); ++FrameIndex)
        {
            const Gaf::FAnimationFrame& Frame = Frames[FrameIndex];
            if (!Valid(Frame.BitmapIndex) || Columns == 0)
            {
                continue;
            }

            const std::string Name(Json["Name"].AsString());
            const std::string Where = std::format("frame {} of animation {}", FrameIndex + 1, Name.empty() ? std::format("{}", AnimationIndex + 1) : Name);
            const uint32_t CellIndex = FirstCell + uint32_t(FrameIndex);
            const uint32_t CellX = CellIndex % Columns * CellWidth;
            const uint32_t CellY = CellIndex / Columns * CellHeight;
            const Gaf::FFrame& Old = (*Bitmaps)[Frame.BitmapIndex - 1];
            const std::optional<Gaf::FFrame> Cell = FrameFromImage(**Image, CellX, CellY, CellWidth, CellHeight, Old.ColorKey);
            if (!Cell)
            {
                return std::unexpected(std::format("{} lies outside its sheet", Where));
            }

            // The cell as extracted: the original bitmap where the original frame put it.
            const bool bSameFrame = AnimationIndex < OldAnimations->size() && FrameIndex < (*OldAnimations)[AnimationIndex].Frames.size() &&
                                    (*OldAnimations)[AnimationIndex].Frames[FrameIndex].BitmapIndex == Frame.BitmapIndex;
            const Gaf::FAnimationFrame& Placed = bSameFrame ? (*OldAnimations)[AnimationIndex].Frames[FrameIndex] : Frame;
            const FPlace OldPlace = FramePlace(Placed, Old);
            constexpr size_t RgbaChannels = 4;
            FImage Reference{ .Width = CellWidth, .Height = CellHeight };
            Reference.Pixels.assign(size_t(CellWidth) * CellHeight * RgbaChannels, 0);
            if (PivotX + OldPlace.X >= 0 && PivotY + OldPlace.Y >= 0)
            {
                Blit(Reference, ImageFromFrame(Old), uint32_t(PivotX + OldPlace.X), uint32_t(PivotY + OldPlace.Y), Placed.bMirrorX, Placed.bMirrorY);
            }

            if (FrameFromImage(Reference, 0, 0, CellWidth, CellHeight, Old.ColorKey)->Pixels == Cell->Pixels)
            {
                continue;
            }

            // The opaque pixels' bounds, a single key pixel at the pivot when the cell was cleared.
            uint32_t Left = CellWidth;
            uint32_t Top = CellHeight;
            uint32_t Right = 0;
            uint32_t Bottom = 0;
            for (uint32_t Row = 0; Row < CellHeight; ++Row)
            {
                for (uint32_t Column = 0; Column < CellWidth; ++Column)
                {
                    if (Cell->Pixels[size_t(Row) * CellWidth + Column] != Old.ColorKey)
                    {
                        Left = std::min(Left, Column);
                        Top = std::min(Top, Row);
                        Right = std::max(Right, Column + 1);
                        Bottom = std::max(Bottom, Row + 1);
                    }
                }
            }

            if (Right == 0)
            {
                Left = uint32_t(std::clamp(PivotX, 0, int32_t(CellWidth) - 1));
                Top = uint32_t(std::clamp(PivotY, 0, int32_t(CellHeight) - 1));
                Right = Left + 1;
                Bottom = Top + 1;
            }

            Gaf::FFrame Bitmap = *FrameFromImage(**Image, CellX + Left, CellY + Top, Right - Left, Bottom - Top, Old.ColorKey);
            if (Frame.bMirrorX || Frame.bMirrorY)
            {
                std::vector<uint16_t> Flipped(Bitmap.Pixels.size());
                for (uint32_t Row = 0; Row < Bitmap.Height; ++Row)
                {
                    for (uint32_t Column = 0; Column < Bitmap.Width; ++Column)
                    {
                        const uint32_t SourceRow = Frame.bMirrorY ? Bitmap.Height - 1 - Row : Row;
                        const uint32_t SourceColumn = Frame.bMirrorX ? Bitmap.Width - 1 - Column : Column;
                        Flipped[size_t(Row) * Bitmap.Width + Column] = Bitmap.Pixels[size_t(SourceRow) * Bitmap.Width + SourceColumn];
                    }
                }

                Bitmap.Pixels = std::move(Flipped);
            }

            // The renderer draws the bitmap at the offset plus the trim (FramePlace), so the trim is what puts it back where it was painted.
            const int32_t PaintedX = int32_t(Left) - PivotX;
            const int32_t PaintedY = int32_t(Top) - PivotY;
            Bitmap.TrimX = Frame.bMirrorX ? Frame.OffsetX - PaintedX - int32_t(Bitmap.Width) : PaintedX - Frame.OffsetX;
            Bitmap.TrimY = Frame.bMirrorY ? Frame.OffsetY - PaintedY - int32_t(Bitmap.Height) : PaintedY - Frame.OffsetY;
            Edits[Frame.BitmapIndex - 1].push_back({ .Bitmap = std::move(Bitmap), .Where = Where });
        }
    }

    // The font pages and the leftover page hold each bitmap at its own size.
    const auto ReadPage = [&](const FJsonValue& Page) -> std::optional<std::string>
    {
        const FResult<FImage>* const Image = Sheet(Page["Image"].AsString());
        if (!Image)
        {
            return std::nullopt;
        }

        if (!*Image)
        {
            return Image->error();
        }

        for (const FJsonValue& Place : Page["Places"].Elements)
        {
            const uint32_t Index = Unsigned(Place["Bitmap"]);
            if (!Valid(Index))
            {
                return std::format("{} places a bitmap {} the GAF does not have", Page["Image"].AsString(), Index);
            }

            const Gaf::FFrame& Old = (*Bitmaps)[Index - 1];
            std::optional<Gaf::FFrame> Cut = FrameFromImage(**Image, Unsigned(Place["X"]), Unsigned(Place["Y"]), Old.Width, Old.Height, Old.ColorKey);
            if (!Cut)
            {
                return std::format("bitmap {} lies outside {}", Index, Page["Image"].AsString());
            }

            // Compared as extracted, since the PNG keeps only RGB555 and the key.
            if (Cut->Pixels != FrameFromImage(ImageFromFrame(Old), 0, 0, Old.Width, Old.Height, Old.ColorKey)->Pixels)
            {
                Cut->TrimX = Old.TrimX;
                Cut->TrimY = Old.TrimY;
                Edits[Index - 1].push_back({ .Bitmap = std::move(*Cut), .Where = std::string(Page["Image"].AsString()) });
            }
        }

        return std::nullopt;
    };
    for (const FJsonValue& Font : Document["Fonts"].Elements)
    {
        if (const std::optional<std::string> Error = ReadPage(Font))
        {
            return std::unexpected(*Error);
        }
    }

    if (const std::optional<std::string> Error = ReadPage(Document["Leftovers"]))
    {
        return std::unexpected(*Error);
    }

    for (size_t Index = 0; Index < Edits.size(); ++Index)
    {
        if (Edits[Index].empty())
        {
            continue;
        }

        for (const FBitmapEdit& Edit : Edits[Index])
        {
            if (!SameBitmap(Edit.Bitmap, Edits[Index].front().Bitmap))
            {
                return std::unexpected(std::format("bitmap {} is edited differently in {} and {}; make every copy the same", Index + 1, Edits[Index].front().Where, Edit.Where));
            }
        }

        (*Bitmaps)[Index] = Edits[Index].front().Bitmap;
    }

    return *Bitmaps;
}

// A GAF rebuilt over the original from its edited files: the sheets and pages cut back into its bitmaps, the JSON's animations and fonts, and the WAV samples.
// The bitmap count and the font count are fixed; the .fnt files only mirror the JSON.
FResult<FBytes> RebuildGaf(const std::span<const uint8_t> Original, const std::vector<FMadeFile>& Files)
{
    std::optional<std::vector<Gaf::FContainerEntry>> Entries = Gaf::ReadContainer(Original);
    const FMadeFile* const Json = FindMade(Files, ".json");
    const std::optional<FJsonValue> Document = Json ? ParseJsonFile(Json->Bytes) : std::nullopt;
    if (!Entries || !Document)
    {
        return std::unexpected(!Entries ? "the original GAF does not parse" : "its JSON is missing or not valid JSON");
    }

    for (const FMadeFile& File : Files)
    {
        if (File.bEdited && Extension(File.Path.string()) == "fnt")
        {
            return std::unexpected(std::format("{} is made from the Fonts in {}; edit those and restore it", File.Path.filename().string(), Json->Path.filename().string()));
        }
    }

    const auto Entry = [&](const char (&Tag)[5]) { return std::ranges::find(*Entries, Gaf::Tag(Tag), &Gaf::FContainerEntry::Tag); };
    if (std::ranges::any_of(Files, [](const FMadeFile& File) { return File.bChanged && Extension(File.Path.string()) == "png"; }))
    {
        FResult<std::vector<Gaf::FFrame>> Frames = BitmapsFromSheets(Original, *Document, Files);
        if (!Frames)
        {
            return std::unexpected(Frames.error());
        }

        if (!Gaf::WriteFrames(*Entries, *Frames))
        {
            return std::unexpected("its bitmaps cannot be written back");
        }
    }

    if (Json->bChanged)
    {
        if (!Gaf::WriteAnimations(*Entries, AnimationsFromJson(*Document)))
        {
            return std::unexpected("its animations cannot be written back");
        }

        const std::optional<std::vector<Font::FFont>> Fonts = Font::LoadFonts(Original);
        const std::optional<std::vector<Font::FFont>> Edited = Fonts ? FontsFromJson(*Document, *Fonts) : std::nullopt;
        if (!Edited || (!Edited->empty() && !Font::WriteFonts(*Entries, *Edited)))
        {
            return std::unexpected("its fonts cannot be written back; the font count and glyph codes are fixed");
        }
    }

    const std::vector<FJsonValue>& Sounds = (*Document)["Sounds"].Elements;
    for (const FMadeFile& File : Files)
    {
        if (!File.bChanged || Extension(File.Path.string()) != "wav")
        {
            continue;
        }

        const auto Listed = std::ranges::find_if(Sounds, [&](const FJsonValue& Name) { return PathFromUtf8(Name.AsString()) == File.Path.filename(); });
        const std::optional<Sound::FSample> Sample = Sound::DecodeWav(File.Bytes);
        const auto Smp = Entry("SMP_");
        constexpr size_t SampleSlot = 1;
        if (Listed == Sounds.end() || !Sample || Smp == Entries->end() || !Sound::WriteGafSample(Smp->Slots[SampleSlot], size_t(Listed - Sounds.begin()), *Sample))
        {
            return std::unexpected(std::format("{} cannot be written back; it must stay 8-bit or 16-bit PCM", File.Path.filename().string()));
        }
    }

    FBytes Rebuilt = Gaf::WriteContainer(*Entries);
    const std::optional<std::vector<Gaf::FFrame>> Frames = Gaf::LoadFrames(Rebuilt);
    if (!Frames || !Gaf::LoadAnimations(Rebuilt, Frames->size()) || !Font::LoadFonts(Rebuilt))
    {
        return std::unexpected("the rebuilt GAF does not load; check the animations' bitmap numbers");
    }

    return Rebuilt;
}

// A Tiled property's value as the text the level files hold.
std::string PropertyText(const FJsonValue& Value)
{
    if (Value.Type != FJsonValue::EType::Number)
    {
        return std::string(Value.AsString());
    }

    const double Number = Value.AsNumber();
    return Number == std::floor(Number) ? std::format("{}", int64_t(Number)) : std::format("{}", Number);
}

const FJsonValue* FindLayer(const FJsonValue& Map, const std::string_view Name)
{
    const auto Found = std::ranges::find_if(Map["layers"].Elements, [&](const FJsonValue& Layer) { return Layer["name"].AsString() == Name; });
    return Found == Map["layers"].Elements.end() ? nullptr : &*Found;
}

using FLevelFiles = std::map<std::string, FBytes>;

// The .3DG over the original: BLK_ blocks cut from the tileset with each tile's Depth, MAP_ layers from the tile layers.
FResult<FBytes> Rebuild3dg(const std::span<const uint8_t> Original, const FJsonValue& Map, const FImage& Tileset, const uint32_t Width, const uint32_t Height)
{
    std::optional<std::vector<Gaf::FContainerEntry>> Entries = Gaf::ReadContainer(Original);
    const auto Blocks = Entries ? std::ranges::find(*Entries, Gaf::Tag("BLK_"), &Gaf::FContainerEntry::Tag) : std::vector<Gaf::FContainerEntry>::iterator{};
    const auto Grid = Entries ? std::ranges::find(*Entries, Gaf::Tag("MAP_"), &Gaf::FContainerEntry::Tag) : std::vector<Gaf::FContainerEntry>::iterator{};
    if (!Entries || Blocks == Entries->end() || Grid == Entries->end())
    {
        return std::unexpected("the original .3dg does not parse");
    }

    const std::optional<std::vector<Gaf::FFrame>> OldBlocks = Gaf::ParseBitmaps(Blocks->Slots[BlockBitmapSlot]);
    const FJsonValue& Set = Map["tilesets"][0];
    const uint32_t TileCount = Unsigned(Set["tilecount"]);
    const uint32_t Columns = Unsigned(Set["columns"]);
    if (!OldBlocks || OldBlocks->empty() || TileCount == 0 || Columns == 0 || Unsigned(Set["firstgid"]) != 1)
    {
        return std::unexpected("the map needs its one tileset, starting at gid 1");
    }

    std::vector<Gaf::FFrame> Frames;
    FBytes Metadata;
    for (uint32_t Tile = 0; Tile < TileCount; ++Tile)
    {
        const uint16_t ColorKey = Tile < OldBlocks->size() ? (*OldBlocks)[Tile].ColorKey : OldBlocks->front().ColorKey;
        std::optional<Gaf::FFrame> Frame = FrameFromImage(Tileset, Tile % Columns * BlockWidth, Tile / Columns * BlockHeight, BlockWidth, BlockHeight, ColorKey);
        const size_t Kept = Tile < OldBlocks->size() ? Tile : 0;
        FBlockMetadata Block = ReadStruct<FBlockMetadata>(Blocks->Slots[BlockMetadataSlot], Kept * sizeof(FBlockMetadata)).value_or(FBlockMetadata{});
        if (!Frame)
        {
            return std::unexpected(std::format("tile {} lies outside the tileset image", Tile));
        }

        for (const FJsonValue& Properties : Set["tiles"].Elements)
        {
            if (Unsigned(Properties["id"]) != Tile)
            {
                continue;
            }

            for (const FJsonValue& Property : Properties["properties"].Elements)
            {
                Block.Depth = Property["name"].AsString() == "Depth" ? Int(Property["value"]) : Block.Depth;
            }
        }

        if (Block.Depth < BackgroundTerrainDepth || Block.Depth > MaxTerrainDepth)
        {
            return std::unexpected(std::format("tile {} has Depth {}; it runs from {} to {}", Tile, Block.Depth, BackgroundTerrainDepth, MaxTerrainDepth));
        }

        const auto* const Bytes = reinterpret_cast<const uint8_t*>(&Block);
        Metadata.insert(Metadata.end(), Bytes, Bytes + sizeof(Block));
        Frames.push_back(std::move(*Frame));
    }

    std::optional<FBytes> Stream = Gaf::WriteBitmaps(Blocks->Slots[BlockBitmapSlot], Frames);
    if (!Stream)
    {
        return std::unexpected("the blocks cannot be written back");
    }

    Blocks->Slots[BlockBitmapSlot] = std::move(*Stream);
    Blocks->Slots[BlockMetadataSlot] = std::move(Metadata);

    // Tiled keeps flips in a gid's top bits, which blocks cannot do.
    constexpr uint32_t FlipBits = 0xE0000000;
    std::vector<const FJsonValue*> Layers;
    for (const FJsonValue& Layer : Map["layers"].Elements)
    {
        if (Layer["type"].AsString() == "tilelayer")
        {
            Layers.push_back(&Layer);
        }
    }

    std::optional<FMapHeader> Header = ReadStruct<FMapHeader>(Grid->Slots[MapHeaderSlot]);
    const size_t LayerRecordSize = Grid->Head.Slot2RecordSize;
    if (!Header || Layers.empty() || LayerRecordSize != sizeof(FMapLayerRecord) || Grid->Slots[MapLayerSlot].size() < LayerRecordSize)
    {
        return std::unexpected("the map needs at least one tile layer");
    }

    FBytes Cells;
    FBytes LayerRecords;
    for (size_t Index = 0; Index < Layers.size(); ++Index)
    {
        const FJsonValue& Layer = *Layers[Index];
        if (!Layer["encoding"].AsString().empty() || Layer["data"].Elements.size() != size_t(Width) * Height)
        {
            return std::unexpected(std::format("tile layer {} must be saved as CSV and cover {}x{} cells", Layer["name"].AsString(), Width, Height));
        }

        for (const FJsonValue& Gid : Layer["data"].Elements)
        {
            const uint32_t Block = Unsigned(Gid);
            if ((Block & FlipBits) != 0 || Block > TileCount)
            {
                return std::unexpected(std::format("tile layer {} has a flipped or unknown tile", Layer["name"].AsString()));
            }

            const auto* const Bytes = reinterpret_cast<const uint8_t*>(&Block);
            Cells.insert(Cells.end(), Bytes, Bytes + sizeof(Block));
        }

        // Each layer's record gives its own size, which the original reads the cells by; a new layer copies the last record.
        const size_t Kept = std::min(Index, Grid->Slots[MapLayerSlot].size() / LayerRecordSize - 1);
        FMapLayerRecord Record = *ReadStruct<FMapLayerRecord>(Grid->Slots[MapLayerSlot], Kept * LayerRecordSize);
        Record.CellBytes = Width * Height * uint32_t(sizeof(uint32_t));
        Record.Width = Width;
        Record.Height = Height;
        const auto* const RecordBytes = reinterpret_cast<const uint8_t*>(&Record);
        LayerRecords.insert(LayerRecords.end(), RecordBytes, RecordBytes + sizeof(Record));
    }

    Header->LayerCount = uint32_t(Layers.size());
    Header->GridWidth = Width;
    Header->GridHeight = Height;
    const auto* const HeaderBytes = reinterpret_cast<const uint8_t*>(&*Header);
    Grid->Slots[MapHeaderSlot].assign(HeaderBytes, HeaderBytes + sizeof(FMapHeader));
    Grid->Slots[MapLayerSlot] = std::move(LayerRecords);
    Grid->Slots[MapCellSlot] = std::move(Cells);
    Grid->Head.Slot3Count = uint32_t(Layers.size());
    return Gaf::WriteContainer(*Entries);
}

// A level's five files rebuilt from its Tiled map, tileset and Terrain.json. The Collision and MapAnimations layers only show what the terrain makes.
FResult<FLevelFiles> RebuildLevel(FResourceArchive& Source, const std::string& LevelPath, const std::vector<FMadeFile>& Files, const FCodePage& Page)
{
    const FMadeFile* const MapFile = FindMade(Files, ".tmj");
    const FMadeFile* const TilesetFile = FindMade(Files, "Tileset.png");
    const FMadeFile* const TerrainFile = FindMade(Files, "Terrain.json");
    const std::optional<FJsonValue> Map = MapFile ? ParseJsonFile(MapFile->Bytes) : std::nullopt;
    const std::optional<FJsonValue> Terrain = TerrainFile ? ParseJsonFile(TerrainFile->Bytes) : std::nullopt;
    const std::optional<FImage> Tileset = TilesetFile ? DecodePng(TilesetFile->Bytes) : std::nullopt;
    const std::optional<FBytes> Original3dg = Source.Read(LevelPath + ".3dg");
    if (!Map || !Terrain || !Tileset || !Original3dg)
    {
        return std::unexpected(!Map ? "its .tmj is missing or not valid JSON" : !Terrain ? "its Terrain.json is missing or not valid JSON" : !Tileset ? "its Tileset.png is not readable" : "the original .3dg is missing");
    }

    std::vector<std::pair<std::string, std::string>> Properties;
    for (const FJsonValue& Property : (*Map)["properties"].Elements)
    {
        Properties.emplace_back(Property["name"].AsString(), PropertyText(Property["value"]));
    }

    const auto Property = [&](const std::string_view Name)
    {
        const auto Found = std::ranges::find(Properties, Name, &std::pair<std::string, std::string>::first);
        return Found == Properties.end() ? std::string() : Found->second;
    };
    const uint32_t ExtentX = uint32_t(std::strtoul(Property("WorldExtentX").c_str(), nullptr, 10));
    const uint32_t ExtentY = uint32_t(std::strtoul(Property("WorldExtentY").c_str(), nullptr, 10));
    const uint32_t Width = Unsigned((*Map)["width"]);
    const uint32_t Height = Unsigned((*Map)["height"]);
    if (ExtentX > UINT16_MAX || ExtentY > UINT16_MAX || Width != GridWidthFromExtent(ExtentX) || Height != GridHeightFromExtent(ExtentY))
    {
        return std::unexpected(std::format("a {}x{} map needs WorldExtentX {} to {} and WorldExtentY {} to {}", Width, Height, (Width - 1) * BlockWidth,
            Width * BlockWidth - 1, (Height - 1) * BlockHeight, Height * BlockHeight - 1));
    }

    FLevelFiles Out;
    FResult<FBytes> Graphics = Rebuild3dg(*Original3dg, *Map, *Tileset, Width, Height);
    if (!Graphics)
    {
        return std::unexpected(Graphics.error());
    }

    Out["3dg"] = std::move(*Graphics);

    // .3DI: the extents, then a record for every filled slot.
    const std::vector<FJsonValue>& Slots = (*Terrain)["Slots"].Elements;
    constexpr size_t TerrainSlots = 4;
    if (Unsigned((*Terrain)["Width"]) != Width || Unsigned((*Terrain)["Height"]) != Height || Slots.size() != TerrainSlots ||
        std::ranges::any_of(Slots, [&](const FJsonValue& Slot) { return Slot.Elements.size() != size_t(Width) * Height; }))
    {
        return std::unexpected(std::format("Terrain.json must be {}x{} like the map, with {} slot arrays", Width, Height, TerrainSlots));
    }

    // LevelBuildWalkGrid (0041ed30) marks each terrain cell in a grid of the map's size and also in the next column and the next two rows,
    // so terrain there writes past the grid and crashes the original; no original level has any.
    constexpr uint32_t ClearColumns = 1;
    constexpr uint32_t ClearRows = 2;
    for (const FJsonValue& Slot : Slots)
    {
        for (uint32_t Y = 0; Y < Height; ++Y)
        {
            for (uint32_t X = 0; X < Width; ++X)
            {
                if (Int(Slot.Elements[size_t(Y) * Width + X]) != 0 && (X + ClearColumns >= Width || Y + ClearRows >= Height))
                {
                    return std::unexpected(std::format("Terrain.json has terrain at cell {}, {}; the last column and the last two rows must stay empty, or the original crashes", X, Y));
                }
            }
        }
    }

    FBytes Cells;
    const auto AppendStruct = [](FBytes& Target, const auto& Value)
    {
        const auto* const Bytes = reinterpret_cast<const uint8_t*>(&Value);
        Target.insert(Target.end(), Bytes, Bytes + sizeof(Value));
    };
    AppendStruct(Cells, F3diHeader{ .WorldExtentX = uint16_t(ExtentX), .WorldExtentY = uint16_t(ExtentY) });
    for (uint32_t X = 0; X < Width; ++X)
    {
        for (uint32_t Y = 0; Y < Height; ++Y)
        {
            for (const FJsonValue& Slot : Slots)
            {
                const int32_t Tile = Int(Slot.Elements[size_t(Y) * Width + X]);
                if (Tile != 0)
                {
                    AppendStruct(Cells, F3diRecord{ .X = uint16_t(X), .Y = uint16_t(Y), .TileId = int16_t(Tile) });
                }
            }
        }
    }

    Out["3di"] = std::move(Cells);

    // .3DX: the animated GAFs and the descriptors, as the editor wrote them.
    const std::vector<FJsonValue>& Gafs = (*Terrain)["Gafs"].Elements;
    const std::vector<FJsonValue>& Descriptors = (*Terrain)["Descriptors"].Elements;
    std::string Descriptions = std::format("################\n1.0\n################\n# GAF files\n#\n{} # Count\n#\n", Gafs.size());
    for (const FJsonValue& Gaf : Gafs)
    {
        Descriptions += std::format("{}\n", Gaf["Source"].AsString());
    }

    Descriptions += std::format("#\n# Objects\n#\n{} # Count\n#\n", Descriptors.size());
    for (size_t Index = 0; Index < Descriptors.size(); ++Index)
    {
        const FJsonValue& Descriptor = Descriptors[Index];
        if (Unsigned(Descriptor["Index"]) != Index + 1 || Unsigned(Descriptor["Gaf"]) > Gafs.size() || Descriptor["Name"].AsString().empty())
        {
            return std::unexpected(std::format("Terrain.json descriptor {} must have Index {}, a Gaf from 0 to {} and a Name", Index + 1, Index + 1, Gafs.size()));
        }

        Descriptions += std::format("{} {}\n", Unsigned(Descriptor["Gaf"]), Descriptor["Name"].AsString());
    }

    // .3DO: the map's own properties as its header, then the Objects layer in order.
    FLevelPropertyBlock Objects;
    const FJsonValue* const ObjectLayer = FindLayer(*Map, "Objects");
    const size_t ObjectCount = ObjectLayer ? (*ObjectLayer)["objects"].Elements.size() : 0;
    constexpr std::array<std::string_view, 4> MapOnly = { "Format", "WorldExtentX", "WorldExtentY", "Terrain" };
    for (const auto& [Name, Value] : Properties)
    {
        if (std::ranges::find(MapOnly, Name) == MapOnly.end())
        {
            Objects.Fields.emplace_back(Name, Name == "OBJECT-COUNT" ? std::format("{}", ObjectCount) : Value);
        }
    }

    for (size_t Index = 0; ObjectLayer && Index < ObjectCount; ++Index)
    {
        const FJsonValue& Object = (*ObjectLayer)["objects"][Index];
        std::string_view ClassName = Object["type"].AsString();
        ClassName = ClassName.empty() ? Object["class"].AsString() : ClassName;
        ClassName = ClassName.empty() ? Object["name"].AsString() : ClassName;
        FLevelPropertyBlock& Block = Objects.Children.emplace_back(FLevelPropertyBlock{ .Kind = "CREATE-OBJECT" });
        Block.Fields.emplace_back("Class-name", ClassName);
        Block.Fields.emplace_back("Position", std::format("{:.3f}, {:.3f}", Object["x"].AsNumber(), Object["y"].AsNumber()));
        for (const FJsonValue& Field : Object["properties"].Elements)
        {
            const std::string_view Name = Field["name"].AsString();
            if (Name == "Blocks")
            {
                const std::optional<FJsonValue> Children = ParseJson(Field["value"].AsString());
                if (!Children)
                {
                    return std::unexpected(std::format("object {} ({}) has Blocks that are not valid JSON", Index + 1, ClassName));
                }

                for (const FJsonValue& Child : Children->Elements)
                {
                    Block.Children.push_back(BlockFromJson(Child));
                }
            }
            else if (Name != "Spawn")
            {
                Block.Fields.emplace_back(Name, PropertyText(Field["value"]));
            }
        }
    }

    // .3DP: the PatrolPaths layer, each path from its object's position and polyline.
    FLevelPropertyBlock Paths;
    if (const FJsonValue* const PathLayer = FindLayer(*Map, "PatrolPaths"))
    {
        for (const FJsonValue& Object : (*PathLayer)["objects"].Elements)
        {
            FLevelPropertyBlock& Block = Paths.Children.emplace_back(FLevelPropertyBlock{ .Kind = "CREATE-PATH" });
            std::string Id;
            for (const FJsonValue& Field : Object["properties"].Elements)
            {
                Id = Field["name"].AsString() == "Id" ? PropertyText(Field["value"]) : Id;
            }

            Block.Fields.emplace_back("Id", Id);
            Block.Fields.emplace_back("Count", std::format("{}", Object["polyline"].Elements.size()));
            for (const FJsonValue& Point : Object["polyline"].Elements)
            {
                Block.Fields.emplace_back("Point", std::format("{}, {}", std::lround(Object["x"].AsNumber() + Point["x"].AsNumber()),
                    std::lround(Object["y"].AsNumber() + Point["y"].AsNumber())));
            }
        }
    }

    const std::string Format = Property("Format");
    for (const auto& [Kind, Text] : { std::pair("3dx", Descriptions), std::pair("3do", BlockSource(Objects, Format, LevelStyle)), std::pair("3dp", BlockSource(Paths, Format, LevelStyle)) })
    {
        std::optional<FBytes> Bytes = FromUtf8(Text, Page);
        if (!Bytes)
        {
            return std::unexpected(std::format("its .{} text has characters {} cannot hold", Kind, &Page == &Cp1251() ? "Windows-1251" : "Windows-1252"));
        }

        Out[Kind] = std::move(*Bytes);
    }

    return Out;
}

// The demo back as DemoLoad (004292b0) reads it: the record count, then the records.
std::optional<FBytes> RecordingFromJson(const std::span<const uint8_t> Json)
{
    const std::optional<FJsonValue> Document = ParseJsonFile(Json);
    if (!Document)
    {
        return std::nullopt;
    }

    const std::vector<FJsonValue>& Records = (*Document)["Records"].Elements;
    FBytes Out;
    const auto Append = [&](const auto& Value)
    {
        const auto* const Bytes = reinterpret_cast<const uint8_t*>(&Value);
        Out.insert(Out.end(), Bytes, Bytes + sizeof(Value));
    };
    Append(uint32_t(Records.size()));
    for (const FJsonValue& Record : Records)
    {
        Append(FDemoRecord{
            .Time = Unsigned(Record["Time"]),
            .Input = Unsigned(Record["Input"]),
            .X = float(Record["X"].AsNumber()),
            .Y = float(Record["Y"].AsNumber()),
            .Direction = Int(Record["Direction"]),
        });
    }

    return Out;
}

struct FPackResult
{
    size_t Unchanged = 0;
    size_t Rebuilt = 0;
    size_t Failed = 0;
};

// One archive of the manifest, packed into Output. With bRebuildAll every entry with a converter is rebuilt from its files, edited or not.
FPackResult PackArchive(const fs::path& Extracted, const FJsonValue& Archive, const fs::path& Output, const bool bRebuildAll)
{
    FPackResult Result;
    FResourceArchive Source;
    const fs::path SourcePath = PathFromUtf8(Archive["Source"].AsString());
    if (!Source.Open(SourcePath))
    {
        std::printf("FAIL cannot open the source archive %s\n", SourcePath.string().c_str());
        ++Result.Failed;
        return Result;
    }

    const FCodePage& Page = CodePageNamed(Archive["TextEncoding"].AsString());
    std::vector<std::pair<std::string, FBytes>> Files;
    std::map<std::string, FResult<FLevelFiles>> Levels;
    for (const std::string& Name : Source.GetStoredNames())
    {
        std::string Key = Lower(Name);
        std::ranges::replace(Key, '/', '\\');
        const auto Entry = std::ranges::find_if(Archive["Entries"].Elements, [&](const FJsonValue& Candidate) { return Candidate["Name"].AsString() == Key; });
        std::optional<FBytes> Original = Source.Read(Name);
        if (!Original)
        {
            std::printf("FAIL cannot read %s from the source archive\n", Name.c_str());
            ++Result.Failed;
            continue;
        }

        // An entry is edited when any file made from it is missing or no longer hashes the same.
        bool bEdited = false;
        std::vector<FMadeFile> Made;
        for (const FJsonValue& File : Entry == Archive["Entries"].Elements.end() ? std::vector<FJsonValue>{} : (*Entry)["Files"].Elements)
        {
            const fs::path Path = Extracted / PathFromUtf8(File["Path"].AsString());
            std::optional<FBytes> Bytes = ReadFile(Path);
            const bool bChanged = !Bytes || std::format("{:016x}", HashBytes(*Bytes)) != File["Hash"].AsString();
            bEdited = bEdited || bChanged;
            Made.push_back({ .Path = Path, .Bytes = Bytes.value_or(FBytes{}), .bEdited = bChanged, .bChanged = bChanged || bRebuildAll });
        }

        const std::string Kind = Extension(Name);
        constexpr std::array<std::string_view, 5> LevelKinds = { "3dx", "3di", "3do", "3dp", "3dg" };
        const bool bLevel = std::ranges::find(LevelKinds, Kind) != LevelKinds.end();
        const bool bConvertible = Made.size() == 1 && (Kind == "wav" || Kind == "txt" || Kind == "tga" || Kind == "3dc" || Kind == "3dr");
        if (Made.empty() || (!bEdited && !(bRebuildAll && (bConvertible || bLevel || Kind == "gaf"))))
        {
            Files.emplace_back(Name, std::move(*Original));
            ++Result.Unchanged;
            continue;
        }

        FResult<FBytes> Rebuilt = std::unexpected(std::format("editing .{} files is not supported yet; restore {}", Kind, Made.front().Path.filename().string()));
        const auto Converted = [&](std::optional<FBytes> Bytes)
        {
            return Bytes ? FResult<FBytes>(std::move(*Bytes)) : std::unexpected(std::format("{} cannot be converted back", Made.front().Path.string()));
        };
        if (bConvertible && Kind == "wav")
        {
            Rebuilt = Made.front().Bytes;
        }
        else if (bConvertible && Kind == "txt")
        {
            Rebuilt = Converted(FromUtf8(AsText(Made.front().Bytes), Page));
        }
        else if (bConvertible && Kind == "tga")
        {
            Rebuilt = Converted(TgaFromPng(Made.front().Bytes, *Original));
        }
        else if (bConvertible && Kind == "3dc")
        {
            const std::optional<std::string> Text = BlocksText(Made.front().Bytes);
            Rebuilt = Converted(Text ? FromUtf8(*Text, Page) : std::nullopt);
        }
        else if (bConvertible && Kind == "3dr")
        {
            Rebuilt = Converted(RecordingFromJson(Made.front().Bytes));
        }
        else if (Kind == "gaf")
        {
            Rebuilt = RebuildGaf(*Original, Made);
        }
        else if (bLevel)
        {
            const std::string LevelPath = Key.substr(0, Key.rfind('.'));
            auto Level = Levels.find(LevelPath);
            if (Level == Levels.end())
            {
                Level = Levels.emplace(LevelPath, RebuildLevel(Source, LevelPath, Made, Page)).first;
            }

            Rebuilt = Level->second.and_then([&](const FLevelFiles& Built) -> FResult<FBytes> { return Built.at(Kind); });
        }

        if (!Rebuilt)
        {
            std::printf("FAIL %s: %s\n", Name.c_str(), Rebuilt.error().c_str());
            ++Result.Failed;
            continue;
        }

        if (!bRebuildAll)
        {
            std::printf("Rebuilt %s\n", Name.c_str());
        }

        Files.emplace_back(Name, std::move(*Rebuilt));
        ++Result.Rebuilt;
    }

    std::error_code Error;
    fs::create_directories(Output, Error);
    const fs::path Target = Output / PathFromUtf8(Archive["File"].AsString());
    if (Result.Failed == 0 && !WriteResourceArchive(Target, Files))
    {
        std::printf("FAIL cannot write %s\n", Target.string().c_str());
        ++Result.Failed;
    }

    return Result;
}

int PackTree(const fs::path& Extracted, const std::optional<fs::path>& Output, const bool bRebuildAll)
{
    const std::optional<FBytes> ManifestBytes = ReadFile(Extracted / "Manifest.json");
    const std::optional<FJsonValue> Manifest = ManifestBytes ? ParseJsonFile(*ManifestBytes) : std::nullopt;
    if (!Manifest)
    {
        std::fprintf(stderr, "No readable Manifest.json in %s; run --extract first\n", Extracted.string().c_str());
        return 1;
    }

    const fs::path Target = Output.value_or(Extracted.parent_path() / "Packed");
    FPackResult Total;
    for (const FJsonValue& Archive : (*Manifest)["Archives"].Elements)
    {
        std::printf("Packing %s\n", std::string(Archive["File"].AsString()).c_str());
        const FPackResult Result = PackArchive(Extracted, Archive, Target, bRebuildAll);
        Total.Unchanged += Result.Unchanged;
        Total.Rebuilt += Result.Rebuilt;
        Total.Failed += Result.Failed;
    }

    std::printf("Packed to %s: %zu unchanged, %zu rebuilt, %zu failed\n", Target.string().c_str(), Total.Unchanged, Total.Rebuilt, Total.Failed);
    return Total.Failed == 0 ? 0 : 1;
}

bool SameFrames(const std::vector<Gaf::FFrame>& A, const std::vector<Gaf::FFrame>& B)
{
    return std::ranges::equal(A, B, [](const Gaf::FFrame& X, const Gaf::FFrame& Y)
    {
        return X.Width == Y.Width && X.Height == Y.Height && X.TrimX == Y.TrimX && X.TrimY == Y.TrimY && X.ColorKey == Y.ColorKey && X.Pixels == Y.Pixels;
    });
}

// What the game reads from a GAF, compared between two copies.
std::string CompareGaf(const std::span<const uint8_t> A, const std::span<const uint8_t> B)
{
    // Every decoded slot and table-of-contents word but the data offset matches too, so parts the port does not read are kept; samples are rewritten whole.
    const std::optional<std::vector<Gaf::FContainerEntry>> EntriesA = Gaf::ReadContainer(A);
    const std::optional<std::vector<Gaf::FContainerEntry>> EntriesB = Gaf::ReadContainer(B);
    const auto SameEntry = [](const Gaf::FContainerEntry& X, const Gaf::FContainerEntry& Y)
    {
        Gaf::FHeadDescriptor HeadY = Y.Head;
        HeadY.DataOffset = X.Head.DataOffset;
        return X.Tag == Y.Tag && std::memcmp(&X.Head, &HeadY, sizeof(HeadY)) == 0 && (X.Tag == Gaf::Tag("SMP_") || X.Slots == Y.Slots);
    };
    if (!EntriesA || !EntriesB || !std::ranges::equal(*EntriesA, *EntriesB, SameEntry))
    {
        return "entries";
    }

    const std::optional<std::vector<Gaf::FFrame>> FramesA = Gaf::LoadFrames(A);
    const std::optional<std::vector<Gaf::FFrame>> FramesB = Gaf::LoadFrames(B);
    if (FramesA.has_value() != FramesB.has_value() || (FramesA && !SameFrames(*FramesA, *FramesB)))
    {
        return "bitmaps";
    }

    const size_t BitmapCount = FramesA ? FramesA->size() : 0;
    const std::optional<std::vector<Gaf::FAnimation>> AnimationsA = Gaf::LoadAnimations(A, BitmapCount);
    const std::optional<std::vector<Gaf::FAnimation>> AnimationsB = Gaf::LoadAnimations(B, BitmapCount);
    const auto SameFrame = [](const Gaf::FAnimationFrame& X, const Gaf::FAnimationFrame& Y)
    {
        return X.BitmapIndex == Y.BitmapIndex && X.OffsetX == Y.OffsetX && X.OffsetY == Y.OffsetY && X.DrawFlags == Y.DrawFlags && X.bMirrorX == Y.bMirrorX &&
               X.bMirrorY == Y.bMirrorY && X.EventFlags == Y.EventFlags && X.EventValue == Y.EventValue && X.Duration == Y.Duration;
    };
    const auto SameAnimation = [&](const Gaf::FAnimation& X, const Gaf::FAnimation& Y)
    {
        return X.Name == Y.Name && X.LoopStart == Y.LoopStart && X.Flags == Y.Flags && std::ranges::equal(X.Frames, Y.Frames, SameFrame);
    };
    if (AnimationsA.has_value() != AnimationsB.has_value() || (AnimationsA && !std::ranges::equal(*AnimationsA, *AnimationsB, SameAnimation)))
    {
        return "animations";
    }

    const std::optional<std::vector<Sound::FSample>> SamplesA = Sound::DecodeGafSamples(A);
    const std::optional<std::vector<Sound::FSample>> SamplesB = Sound::DecodeGafSamples(B);
    const auto SameSample = [](const Sound::FSample& X, const Sound::FSample& Y) { return X.Rate == Y.Rate && X.ChannelCount == Y.ChannelCount && X.Data == Y.Data; };
    if (SamplesA.has_value() != SamplesB.has_value() || (SamplesA && !std::ranges::equal(*SamplesA, *SamplesB, SameSample)))
    {
        return "samples";
    }

    const std::optional<std::vector<Font::FFont>> FontsA = Font::LoadFonts(A);
    const std::optional<std::vector<Font::FFont>> FontsB = Font::LoadFonts(B);
    const auto SameLayer = [](const Font::FGlyphLayer& X, const Font::FGlyphLayer& Y) { return X.BitmapIndex == Y.BitmapIndex && X.OffsetX == Y.OffsetX && X.OffsetY == Y.OffsetY; };
    const auto SameFont = [&](const Font::FFont& X, const Font::FFont& Y)
    {
        return X.Flags == Y.Flags && X.LineGap == Y.LineGap && X.CharSpacing == Y.CharSpacing && X.Advance == Y.Advance && X.LineHeight == Y.LineHeight &&
               X.Baseline == Y.Baseline && X.DrawMode == Y.DrawMode && X.Color.Alpha == Y.Color.Alpha && X.Color.Diffuse == Y.Color.Diffuse &&
               X.Color.Specular == Y.Color.Specular && std::ranges::equal(X.Glyphs, Y.Glyphs, [&](const Font::FGlyph& P, const Font::FGlyph& Q) {
                   return SameLayer(P.Base, Q.Base) && SameLayer(P.Overlay, Q.Overlay);
               });
    };
    if (FontsA.has_value() != FontsB.has_value() || (FontsA && !std::ranges::equal(*FontsA, *FontsB, SameFont)))
    {

        return "fonts";
    }

    return {};
}

// What the game reads from a level, compared between two archives; the .3DI's records that name no descriptor are not kept.
std::string CompareLevel(FResourceArchive& ArchiveA, FResourceArchive& ArchiveB, const std::string& LevelPath)
{
    const std::expected<FLevel, std::string> A = LoadLevel(ArchiveA, LevelPath);
    const std::expected<FLevel, std::string> B = LoadLevel(ArchiveB, LevelPath);
    if (!A || !B)
    {
        return !A ? A.error() : B.error();
    }

    const auto SameObject = [](const FLevelObject& X, const FLevelObject& Y)
    {
        return X.ClassName == Y.ClassName && X.X == Y.X && X.Y == Y.Y && SameBlocks(X.Properties, Y.Properties) && X.CarriedNames == Y.CarriedNames;
    };
    const auto SamePath = [](const FPatrolPath& X, const FPatrolPath& Y) { return X.Id == Y.Id && X.Points == Y.Points; };
    const std::pair<bool, std::string_view> Checks[] = {
        { A->Name == B->Name && A->Id == B->Id && A->GraphicsName == B->GraphicsName, "header" },
        { SameFrames(A->Blocks, B->Blocks) && A->BlockDepths == B->BlockDepths, "blocks" },
        { A->LayerCount == B->LayerCount && A->GridWidth == B->GridWidth && A->GridHeight == B->GridHeight && A->Map == B->Map, "map" },
        { A->AnimGafs == B->AnimGafs && A->TileTypes == B->TileTypes && A->TileReferences == B->TileReferences, "descriptors" },
        { A->WorldExtentX == B->WorldExtentX && A->WorldExtentY == B->WorldExtentY && A->Cells == B->Cells, "terrain cells" },
        { std::ranges::equal(A->Objects, B->Objects, SameObject), "objects" },
        { std::ranges::equal(A->PatrolPaths, B->PatrolPaths, SamePath), "patrol paths" },
    };
    for (const auto& [bSame, Part] : Checks)
    {
        if (!bSame)
        {
            return std::string(Part);
        }
    }

    return {};
}
}

int RunCompress(const std::filesystem::path& Extracted, const std::optional<std::filesystem::path>& Output)
{
    AttachParentConsole();
    return PackTree(Extracted, Output, false);
}

int VerifyExtractRoundTrip(const std::filesystem::path& GameFolder)
{
    const fs::path Work = fs::temp_directory_path() / "OpenVyruzRoundTrip";
    std::error_code Error;
    fs::remove_all(Work, Error);
    const fs::path Extracted = Work / "Extracted";
    const fs::path Packed = Work / "Packed";
    const fs::path Rebuilt = Work / "Rebuilt";
    if (RunExtract(GameFolder, Extracted) != 0 || RunCompress(Extracted, Packed) != 0)
    {
        std::printf("Round trip: FAIL, extraction or packing failed\n");
        return 1;
    }

    // Every entry of every archive reads back the same.
    size_t Compared = 0;
    for (const fs::directory_entry& Item : fs::directory_iterator(Packed, Error))
    {
        FResourceArchive Original;
        FResourceArchive Repacked;
        if (!Original.Open(GameFolder / Item.path().filename()) || !Repacked.Open(Item.path()) || Original.GetStoredNames() != Repacked.GetStoredNames())
        {
            std::printf("Round trip: FAIL, %s does not reopen with the same entries\n", Item.path().filename().string().c_str());
            return 1;
        }

        for (const std::string& Name : Original.GetStoredNames())
        {
            if (Original.Read(Name) != Repacked.Read(Name))
            {
                std::printf("Round trip: FAIL, %s differs\n", Name.c_str());
                return 1;
            }

            ++Compared;
        }
    }

    // Every class library, GAF and level rebuilt from its files reads the same as the original.
    if (PackTree(Extracted, Rebuilt, true) != 0)
    {
        std::printf("Round trip: FAIL, rebuilding every entry failed\n");
        return 1;
    }

    size_t Libraries = 0;
    size_t Gafs = 0;
    size_t Levels = 0;
    size_t Recordings = 0;
    size_t Images = 0;
    for (const fs::directory_entry& Item : fs::directory_iterator(Rebuilt, Error))
    {
        FResourceArchive Original;
        FResourceArchive Repacked;
        Original.Open(GameFolder / Item.path().filename());
        Repacked.Open(Item.path());
        for (const std::string& Name : Original.GetStoredNames())
        {
            const std::string Kind = Extension(Name);
            const std::optional<FBytes> Before = Original.Read(Name);
            const std::optional<FBytes> After = Repacked.Read(Name);
            std::string Difference;
            if (!Before || !After)
            {
                Difference = "unreadable";
            }
            else if (Kind == "3dc")
            {
                const std::expected<FLevelPropertyBlock, std::string> BlocksBefore = ParseLevelBlocks(ToUtf8(*Before, Cp1252()));
                const std::expected<FLevelPropertyBlock, std::string> BlocksAfter = ParseLevelBlocks(ToUtf8(*After, Cp1252()));
                Difference = BlocksBefore && BlocksAfter && SameBlocks(*BlocksBefore, *BlocksAfter) ? "" : "classes";
                ++Libraries;
            }
            else if (Kind == "gaf")
            {
                Difference = CompareGaf(*Before, *After);
                ++Gafs;
            }
            else if (Kind == "tga")
            {
                // Same pixels, and the same bytes unless the original was run-length coded by another encoder.
                const std::optional<FTgaImage> ImageBefore = DecodeTga(*Before);
                const std::optional<FTgaImage> ImageAfter = DecodeTga(*After);
                constexpr size_t TypeOffset = 2;
                constexpr uint8_t RunLengthType = 10;
                const bool bSamePixels = ImageBefore && ImageAfter && ImageBefore->Pixels == ImageAfter->Pixels;
                Difference = bSamePixels && ((*Before)[TypeOffset] == RunLengthType || *Before == *After) ? "" : "image";
                ++Images;
            }
            else if (Kind == "3dr")
            {
                Difference = *Before == *After ? "" : "records";
                ++Recordings;
            }
            else if (Kind == "3do")
            {
                std::string LevelPath = Lower(Name);
                LevelPath.resize(LevelPath.rfind('.'));
                Difference = CompareLevel(Original, Repacked, LevelPath);
                ++Levels;
            }

            if (!Difference.empty())
            {
                std::printf("Round trip: FAIL, %s rebuilt from its files differs in its %s\n", Name.c_str(), Difference.c_str());
                return 1;
            }
        }
    }

    // An edited text, image and sprite come back through their converters; the sprite gets a pixel at its first cell's corner, outside its bitmap.
    const fs::path TextPath = Extracted / "Text" / "Start.txt";
    const fs::path ImagePath = Extracted / "Common" / "Extra" / "Story.png";
    const fs::path SpriteFolder = Extracted / "Common" / "Object" / "Vyruz" / "Vz";
    std::optional<FBytes> TextBytes = ReadFile(TextPath);
    const std::optional<FBytes> ImageBytes = ReadFile(ImagePath);
    std::optional<FImage> Image = ImageBytes ? DecodePng(*ImageBytes) : std::nullopt;
    const std::optional<FBytes> SheetBytes = ReadFile(SpriteFolder / "Vz.png");
    std::optional<FImage> Sheet = SheetBytes ? DecodePng(*SheetBytes) : std::nullopt;
    const std::optional<FBytes> SpriteJson = ReadFile(SpriteFolder / "Vz.json");
    const std::optional<FJsonValue> Sprite = SpriteJson ? ParseJsonFile(*SpriteJson) : std::nullopt;
    const std::vector<FJsonValue> NoAnimations;
    const std::vector<FJsonValue>& SpriteAnimationJson = Sprite ? (*Sprite)["Animations"].Elements : NoAnimations;
    const auto Rot = std::ranges::find_if(SpriteAnimationJson, [](const FJsonValue& Animation) { return Animation["Name"].AsString() == "rot"; });
    constexpr size_t RgbaChannels = 4;
    constexpr size_t AlphaChannel = 3;
    const uint32_t RotCell = Rot == SpriteAnimationJson.end() ? 0 : Unsigned((*Rot)["FirstCell"]);
    const uint32_t SpriteColumns = Sprite ? std::max(1u, Unsigned((*Sprite)["Columns"])) : 1;
    const size_t CornerPixel = Sheet && Sprite ? (size_t(RotCell / SpriteColumns * Unsigned((*Sprite)["CellHeight"])) * Sheet->Width +
                                                     RotCell % SpriteColumns * Unsigned((*Sprite)["CellWidth"])) * RgbaChannels
                                               : 0;
    if (!TextBytes || !Image || !Sheet || Rot == SpriteAnimationJson.end() || CornerPixel >= Sheet->Pixels.size() || Sheet->Pixels[CornerPixel + AlphaChannel] != 0)
    {
        std::printf("Round trip: FAIL, the files to edit are missing\n");
        return 1;
    }

    constexpr std::string_view Added = " - edited";
    TextBytes->insert(TextBytes->end(), Added.begin(), Added.end());
    Image->Pixels[0] = uint8_t(~Image->Pixels[0]);
    const std::optional<FBytes> EditedPng = EncodePng(*Image);
    constexpr std::array<uint8_t, 4> Red = { 0xff, 0, 0, 0xff };
    constexpr uint16_t Red555 = 0x7c00;
    std::ranges::copy(Red, Sheet->Pixels.begin() + ptrdiff_t(CornerPixel));
    const std::optional<FBytes> EditedSheet = EncodePng(*Sheet);
    if (!WriteFile(TextPath, *TextBytes) || !EditedPng || !WriteFile(ImagePath, *EditedPng) || !EditedSheet || !WriteFile(SpriteFolder / "Vz.png", *EditedSheet) ||
        RunCompress(Extracted, Packed) != 0)
    {
        std::printf("Round trip: FAIL, the edited tree did not pack\n");
        return 1;
    }

    FResourceArchive Data;
    Data.Open(GameFolder / "vz-data.rsc");
    FResourceArchive Edited;
    Edited.Open(Packed / "vz-data.rsc");
    const std::optional<FBytes> PackedText = Edited.Read("start.txt");
    const std::optional<FBytes> PackedImage = Edited.Read("common\\data\\extra\\story.tga");
    const std::optional<FTgaImage> Decoded = PackedImage ? DecodeTga(*PackedImage) : std::nullopt;
    const std::optional<FBytes> OriginalText = Data.Read("start.txt");
    if (!PackedText || !OriginalText || AsText(*PackedText) != std::string(AsText(*OriginalText)) + std::string(Added) || !Decoded || Decoded->Pixels != Image->Pixels)
    {
        std::printf("Round trip: FAIL, the edited text or image did not come back\n");
        return 1;
    }

    // The painted corner of the rot animation's first cell is the trimmed bitmap's first pixel, drawn at the sheet's pivot.
    const std::optional<FBytes> PackedSprite = Edited.Read("common\\data\\object\\vyruz\\vz.gaf");
    const std::optional<std::vector<Gaf::FFrame>> SpriteFrames = PackedSprite ? Gaf::LoadFrames(*PackedSprite) : std::nullopt;
    const std::optional<std::vector<Gaf::FAnimation>> SpriteAnimations = SpriteFrames ? Gaf::LoadAnimations(*PackedSprite, SpriteFrames->size()) : std::nullopt;
    const size_t RotIndex = size_t(Rot - SpriteAnimationJson.begin());
    const Gaf::FAnimationFrame* const First =
        SpriteAnimations && RotIndex < SpriteAnimations->size() && !(*SpriteAnimations)[RotIndex].Frames.empty() ? &(*SpriteAnimations)[RotIndex].Frames.front() : nullptr;
    const Gaf::FFrame* const Painted = First && First->BitmapIndex >= 1 && First->BitmapIndex <= SpriteFrames->size() ? &(*SpriteFrames)[First->BitmapIndex - 1] : nullptr;
    const FPlace PaintedPlace = Painted ? FramePlace(*First, *Painted) : FPlace{};
    const size_t Corner = Painted ? size_t(First->bMirrorY ? Painted->Height - 1 : 0) * Painted->Width + (First->bMirrorX ? Painted->Width - 1 : 0) : 0;
    if (!Painted || PaintedPlace.X != -Int((*Sprite)["PivotX"]) || PaintedPlace.Y != -Int((*Sprite)["PivotY"]) || Painted->Pixels[Corner] != Red555)
    {
        std::printf("Round trip: FAIL, the painted sprite pixel did not come back where it was drawn\n");
        return 1;
    }

    fs::remove_all(Work, Error);
    std::printf("Round trip: %zu entries read back unchanged; %zu class libraries, %zu GAFs, %zu levels, %zu images and %zu demo rebuilt from their files read the same; an edited text, image and sprite packed\n",
        Compared, Libraries, Gafs, Levels, Images, Recordings);
    return 0;
}
