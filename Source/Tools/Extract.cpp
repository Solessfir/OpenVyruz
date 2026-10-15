#include "Tools/Extract.h"

#include "Game/Animation.h"
#include "Game/Collision.h"
#include "Game/Engine.h"
#include "Game/Level.h"
#include "Pg/Font.h"
#include "Pg/Gaf.h"
#include "Pg/ResourceArchive.h"
#include "Pg/Sound.h"
#include "Pg/Tga.h"
#include "Tools/Json.h"
#include "Tools/Png.h"
#include "Tools/Text.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#endif

namespace
{
namespace fs = std::filesystem;

// A segment in PascalCase: words split at anything not a letter or digit, a plus written out (CPU has both ROOF and ROOF+), and a level id kept in capitals.
std::string Pascal(const std::string_view Segment)
{
    const bool bLevelId = Segment.size() == 4 && (Segment[0] == 't' || Segment[0] == 'T') && std::isdigit(uint8_t(Segment[1])) && std::isalpha(uint8_t(Segment[2])) &&
                          std::isdigit(uint8_t(Segment[3]));
    std::string Out;
    bool bWordStart = true;
    for (const char Character : Segment)
    {
        if (Character == '+')
        {
            Out += "Plus";
            bWordStart = true;
        }
        else if (!std::isalnum(uint8_t(Character)))
        {
            bWordStart = true;
        }
        else
        {
            Out += bWordStart || bLevelId ? char(std::toupper(uint8_t(Character))) : char(std::tolower(uint8_t(Character)));
            bWordStart = false;
        }
    }

    return Out;
}

std::vector<std::string> SplitPath(const std::string_view Name)
{
    std::vector<std::string> Parts;
    size_t Start = 0;
    for (size_t Index = 0; Index <= Name.size(); ++Index)
    {
        if (Index == Name.size() || Name[Index] == '\\' || Name[Index] == '/')
        {
            if (Index > Start)
            {
                Parts.emplace_back(Name.substr(Start, Index - Start));
            }

            Start = Index + 1;
        }
    }

    return Parts;
}

// Where an entry goes, without its extension: a zone's DATA folders lose that level, its level files gather under Levels\<Id>, its class libraries under Classes,
// and the loose texts under Text. Pages such as T1U1-EN.TXT join their level as PageEn.
struct FTarget
{
    fs::path Folder;
    std::string Stem;
    std::string Extension;
};

FTarget MapEntry(const std::string_view Name)
{
    std::vector<std::string> Parts = SplitPath(Name);
    const std::string File = Parts.empty() ? std::string(Name) : Parts.back();
    const size_t Dot = File.rfind('.');
    std::string Stem = Dot == std::string::npos ? File : File.substr(0, Dot);
    std::string Extension = Dot == std::string::npos ? std::string() : File.substr(Dot + 1);
    std::ranges::transform(Extension, Extension.begin(), [](const char Character) { return char(std::tolower(uint8_t(Character))); });
    Parts.pop_back();

    FTarget Target{ .Extension = Extension };
    if (Parts.empty())
    {
        Target.Folder = "Text";
        Target.Stem = Pascal(Stem);
        return Target;
    }

    Target.Folder = Pascal(Parts.front());
    if (Parts.size() == 1)
    {
        const std::string Level = Stem.substr(0, Stem.find('-'));
        const bool bLevelFile = Extension.starts_with("3d") && Extension != "3dc";
        const bool bPage = Extension == "txt" && Level.size() < Stem.size();
        if (bLevelFile || bPage)
        {
            Target.Folder /= "Levels";
            Target.Folder /= Pascal(Level);
            Target.Stem = bPage ? "Page" + Pascal(Stem.substr(Level.size())) : Pascal(Level);
            if (Extension == "3dr")
            {
                Target.Stem = "Recording";
            }

            return Target;
        }

        Target.Folder /= Extension == "3dc" ? "Classes" : "";
        Target.Stem = Pascal(Stem);
        return Target;
    }

    constexpr std::string_view DataFolder = "data";
    for (size_t Index = 1; Index < Parts.size(); ++Index)
    {
        std::string Lower = Parts[Index];
        std::ranges::transform(Lower, Lower.begin(), [](const char Character) { return char(std::tolower(uint8_t(Character))); });
        if (!(Index == 1 && Lower == DataFolder))
        {
            Target.Folder /= Pascal(Parts[Index]);
        }
    }

    Target.Stem = Pascal(Stem);
    return Target;
}

// 16-bit PCM RIFF WAVE from the decoder's floats.
std::vector<uint8_t> EncodeWav(const Sound::FSample& Sample)
{
    constexpr uint16_t PcmFormat = 1;
    constexpr uint16_t BitsPerSample = 16;
    constexpr uint32_t FormatChunkSize = 16;
    constexpr uint32_t HeaderSize = 36;
    const uint16_t Channels = uint16_t(Sample.ChannelCount);
    const uint32_t DataSize = uint32_t(Sample.Data.size() * sizeof(int16_t));
    std::vector<uint8_t> Out;
    const auto Text = [&](const std::string_view Tag) { Out.insert(Out.end(), Tag.begin(), Tag.end()); };
    const auto U32 = [&](const uint32_t Value)
    {
        for (int Shift = 0; Shift < 32; Shift += 8)
        {
            Out.push_back(uint8_t(Value >> Shift));
        }
    };
    const auto U16 = [&](const uint16_t Value)
    {
        Out.push_back(uint8_t(Value));
        Out.push_back(uint8_t(Value >> 8));
    };
    Text("RIFF");
    U32(HeaderSize + DataSize);
    Text("WAVEfmt ");
    U32(FormatChunkSize);
    U16(PcmFormat);
    U16(Channels);
    U32(Sample.Rate);
    U32(Sample.Rate * Channels * (BitsPerSample / 8));
    U16(uint16_t(Channels * (BitsPerSample / 8)));
    U16(BitsPerSample);
    Text("data");
    U32(DataSize);
    for (const float Value : Sample.Data)
    {
        U16(uint16_t(Sound::SampleToPcm16(Value)));
    }

    return Out;
}

struct FWrittenFile
{
    std::string Path;
    uint64_t Hash = 0;
};

struct FManifestEntry
{
    std::string Name;
    uint32_t Size = 0;
    std::string Kind;
    std::vector<FWrittenFile> Files;
    std::string Error;
};

class FExtractor
{
public:
    explicit FExtractor(fs::path InRoot) : Root(std::move(InRoot)) {}

    // Writes a file under the output root and notes it against the entry being extracted.
    bool Write(FManifestEntry& Entry, const fs::path& Relative, const std::span<const uint8_t> Bytes)
    {
        std::error_code Error;
        fs::create_directories((Root / Relative).parent_path(), Error);
        std::ofstream File(Root / Relative, std::ios::binary);
        File.write(reinterpret_cast<const char*>(Bytes.data()), std::streamsize(Bytes.size()));
        if (!File)
        {
            Entry.Error = std::format("cannot write {}", Relative.generic_string());
            return false;
        }

        Entry.Files.push_back({ .Path = Relative.generic_string(), .Hash = HashBytes(Bytes) });
        return true;
    }

    bool WriteText(FManifestEntry& Entry, const fs::path& Relative, const std::string_view Text) { return Write(Entry, Relative, AsBytes(Text)); }

    // A target path not yet taken, so two entries never overwrite each other.
    fs::path Claim(const fs::path& Folder, const std::string& Stem, const std::string_view Kind)
    {
        std::string Unique = Stem;
        for (size_t Suffix = 2; !Claimed.insert(std::format("{}.{}", (Folder / Unique).generic_string(), Kind)).second; ++Suffix)
        {
            Unique = std::format("{}{}", Stem, Suffix);
        }

        return Folder / Unique;
    }

private:
    fs::path Root;
    std::set<std::string> Claimed;
};

// Packs the GAF's bitmaps on shelves, tallest first, one transparent pixel apart so filtering does not bleed.
struct FAtlas
{
    FImage Image;
    // Each bitmap's top-left, by 0-based index.
    std::vector<std::array<uint32_t, 2>> Places;
};

FAtlas PackAtlas(const std::vector<FImage>& Images)
{
    constexpr uint32_t Gap = 1;
    constexpr uint32_t MinWidth = 64;
    constexpr double Slack = 1.15;
    uint64_t Area = 0;
    uint32_t Widest = 0;
    for (const FImage& Image : Images)
    {
        Area += uint64_t(Image.Width + Gap) * (Image.Height + Gap);
        Widest = std::max(Widest, Image.Width + Gap);
    }

    const uint32_t Width = std::max({ MinWidth, Widest, uint32_t(std::ceil(std::sqrt(double(Area) * Slack))) });
    std::vector<size_t> Order(Images.size());
    for (size_t Index = 0; Index < Order.size(); ++Index)
    {
        Order[Index] = Index;
    }

    std::ranges::stable_sort(Order, [&](const size_t A, const size_t B) { return Images[A].Height > Images[B].Height; });
    FAtlas Atlas;
    Atlas.Places.resize(Images.size());
    uint32_t X = 0;
    uint32_t Y = 0;
    uint32_t ShelfHeight = 0;
    for (const size_t Index : Order)
    {
        const FImage& Image = Images[Index];
        if (X + Image.Width > Width)
        {
            X = 0;
            Y += ShelfHeight + Gap;
            ShelfHeight = 0;
        }

        Atlas.Places[Index] = { X, Y };
        X += Image.Width + Gap;
        ShelfHeight = std::max(ShelfHeight, Image.Height);
    }

    constexpr size_t RgbaChannels = 4;
    Atlas.Image = { .Width = Width, .Height = std::max(Y + ShelfHeight, 1u) };
    Atlas.Image.Pixels.assign(size_t(Atlas.Image.Width) * Atlas.Image.Height * RgbaChannels, 0);
    for (size_t Index = 0; Index < Images.size(); ++Index)
    {
        Blit(Atlas.Image, Images[Index], Atlas.Places[Index][0], Atlas.Places[Index][1]);
    }

    return Atlas;
}

// The draw modes in a frame's draw flags (bits 1 to 7), as the renderer blends them.
std::string_view BlendName(const uint32_t DrawFlags)
{
    constexpr uint32_t BlendMask = 0xFE;
    constexpr uint32_t AdditiveMode = 6;
    constexpr uint32_t InverseSourceMode = 8;
    const uint32_t Mode = DrawFlags & BlendMask;
    return Mode == AdditiveMode ? "Additive" : Mode == InverseSourceMode ? "InverseSource" : "Normal";
}

// The bitmaps in an even grid in index order, each at its trim inside a cell that holds them all, so frames cut from one sprite line up.
FAtlas GridAtlas(const std::vector<FImage>& Images, const std::vector<const Gaf::FFrame*>& Bitmaps)
{
    int32_t Left = INT32_MAX;
    int32_t Top = INT32_MAX;
    int32_t Right = INT32_MIN;
    int32_t Bottom = INT32_MIN;
    for (const Gaf::FFrame* const Bitmap : Bitmaps)
    {
        Left = std::min(Left, Bitmap->TrimX);
        Top = std::min(Top, Bitmap->TrimY);
        Right = std::max(Right, Bitmap->TrimX + int32_t(Bitmap->Width));
        Bottom = std::max(Bottom, Bitmap->TrimY + int32_t(Bitmap->Height));
    }

    const uint32_t Count = uint32_t(Images.size());
    const uint32_t Columns = uint32_t(std::ceil(std::sqrt(double(Count))));
    const uint32_t Rows = (Count + Columns - 1) / Columns;
    const uint32_t CellWidth = uint32_t(Right - Left);
    const uint32_t CellHeight = uint32_t(Bottom - Top);
    constexpr size_t RgbaChannels = 4;
    FAtlas Atlas;
    Atlas.Image = { .Width = Columns * CellWidth, .Height = Rows * CellHeight };
    Atlas.Image.Pixels.assign(size_t(Atlas.Image.Width) * Atlas.Image.Height * RgbaChannels, 0);
    for (uint32_t Index = 0; Index < Count; ++Index)
    {
        const std::array<uint32_t, 2> Place = { Index % Columns * CellWidth + uint32_t(Bitmaps[Index]->TrimX - Left),
            Index / Columns * CellHeight + uint32_t(Bitmaps[Index]->TrimY - Top) };
        Blit(Atlas.Image, Images[Index], Place[0], Place[1]);
        Atlas.Places.push_back(Place);
    }

    return Atlas;
}

// Puts a set of bitmaps on one page, packed for a font or in a grid for the bitmaps no animation shows, and lists where each went.
bool WritePage(FExtractor& Extractor, FManifestEntry& Entry, FJsonWriter& Json, const fs::path& Path, const std::vector<FImage>& Images,
    const std::vector<uint32_t>& Indices, std::vector<std::array<uint32_t, 2>>* Places = nullptr, const std::vector<Gaf::FFrame>* GridOf = nullptr)
{
    std::vector<FImage> Members;
    std::vector<const Gaf::FFrame*> MemberBitmaps;
    for (const uint32_t Index : Indices)
    {
        Members.push_back(Images[Index - 1]);
        if (GridOf)
        {
            MemberBitmaps.push_back(&(*GridOf)[Index - 1]);
        }
    }

    const FAtlas Atlas = GridOf ? GridAtlas(Members, MemberBitmaps) : PackAtlas(Members);
    const std::optional<std::vector<uint8_t>> Png = EncodePng(Atlas.Image);
    if (!Png || !Extractor.Write(Entry, Path, *Png))
    {
        Entry.Error = Entry.Error.empty() ? "cannot encode a page" : Entry.Error;
        return false;
    }

    Json.Field("Image", Path.filename().string());
    Json.Key("Places");
    Json.BeginArray();
    for (size_t Member = 0; Member < Indices.size(); ++Member)
    {
        Json.BeginObject();
        Json.Field("Bitmap", Indices[Member]);
        Json.Field("X", Atlas.Places[Member][0]);
        Json.Field("Y", Atlas.Places[Member][1]);
        Json.EndObject();
    }

    Json.EndArray();
    if (Places)
    {
        *Places = Atlas.Places;
    }

    return true;
}

// A GAF as a folder: one sheet of every animation, every cell the same size with the sprite's anchor at the same point and frames in order; a page per font
// with its BMFont file; the bitmaps no animation or font shows on one more page; the samples as WAV; and the JSON tying them together.
bool ExportGaf(FExtractor& Extractor, FManifestEntry& Entry, const fs::path& Folder, const std::span<const uint8_t> File, const FCodePage& Page)
{
    const std::optional<std::vector<Gaf::FFrame>> Frames = Gaf::LoadFrames(File);
    const std::vector<Gaf::FFrame> Bitmaps = Frames.value_or(std::vector<Gaf::FFrame>{});
    const std::optional<std::vector<Gaf::FAnimation>> Animations = Gaf::LoadAnimations(File, Bitmaps.size());
    const std::optional<std::vector<Sound::FSample>> Samples = Sound::DecodeGafSamples(File);
    const std::optional<std::vector<Font::FFont>> Fonts = Font::LoadFonts(File);
    const std::string Stem = Folder.filename().string();

    std::vector<FImage> Images;
    Images.reserve(Bitmaps.size());
    for (const Gaf::FFrame& Bitmap : Bitmaps)
    {
        Images.push_back(ImageFromFrame(Bitmap));
    }

    std::vector<bool> bShown(Bitmaps.size());
    const auto Visible = [&](const uint32_t Index) { return Index >= 1 && Index <= Bitmaps.size(); };
    std::vector<std::string> SoundFiles;
    for (size_t Index = 0; Samples && Index < Samples->size(); ++Index)
    {
        const fs::path Path = Extractor.Claim(Folder, std::format("Sound{}", Index + 1), "wav");
        if (!Extractor.Write(Entry, fs::path(Path).replace_extension(".wav"), EncodeWav((*Samples)[Index])))
        {
            return false;
        }

        SoundFiles.push_back(Path.filename().string() + ".wav");
    }

    FJsonWriter Json;
    Json.BeginObject();
    Json.Field("FrameMilliseconds", AnimationPeriodMilliseconds);
    Json.Key("Bitmaps");
    Json.BeginArray();
    for (const Gaf::FFrame& Bitmap : Bitmaps)
    {
        Json.BeginObject();
        Json.Field("Width", Bitmap.Width);
        Json.Field("Height", Bitmap.Height);
        Json.Field("TrimX", Bitmap.TrimX);
        Json.Field("TrimY", Bitmap.TrimY);
        Json.EndObject();
    }

    Json.EndArray();
    // One sheet for the whole GAF: every cell the size that covers any frame's bitmap where it is drawn, the anchor at the same pivot in each,
    // and each animation from the start of a new row.
    const std::vector<Gaf::FAnimation> AnimationList = Animations.value_or(std::vector<Gaf::FAnimation>{});
    int32_t Left = 0;
    int32_t Top = 0;
    int32_t Right = 0;
    int32_t Bottom = 0;
    bool bAny = false;
    size_t LongestAnimation = 0;
    for (const Gaf::FAnimation& Animation : AnimationList)
    {
        LongestAnimation = std::max(LongestAnimation, Animation.Frames.size());
        for (const Gaf::FAnimationFrame& Frame : Animation.Frames)
        {
            if (!Visible(Frame.BitmapIndex))
            {
                continue;
            }

            const Gaf::FFrame& Bitmap = Bitmaps[Frame.BitmapIndex - 1];
            const FPlace Place = FramePlace(Frame, Bitmap);
            Left = bAny ? std::min(Left, Place.X) : Place.X;
            Top = bAny ? std::min(Top, Place.Y) : Place.Y;
            Right = bAny ? std::max(Right, Place.X + int32_t(Bitmap.Width)) : Place.X + int32_t(Bitmap.Width);
            Bottom = bAny ? std::max(Bottom, Place.Y + int32_t(Bitmap.Height)) : Place.Y + int32_t(Bitmap.Height);
            bAny = true;
        }
    }

    const uint32_t Columns = std::max(1u, uint32_t(std::ceil(std::sqrt(double(LongestAnimation)))));
    const uint32_t CellWidth = bAny ? uint32_t(Right - Left) : 0;
    const uint32_t CellHeight = bAny ? uint32_t(Bottom - Top) : 0;
    // An animation that draws nothing takes no rows; its FirstCell says so.
    constexpr int64_t NotOnSheet = -1;
    std::vector<int64_t> FirstCells;
    uint32_t Rows = 0;
    for (const Gaf::FAnimation& Animation : AnimationList)
    {
        const bool bDraws = std::ranges::any_of(Animation.Frames, [&](const Gaf::FAnimationFrame& Frame) { return Visible(Frame.BitmapIndex); });
        FirstCells.push_back(bDraws ? int64_t(Rows) * Columns : NotOnSheet);
        Rows += bDraws ? (uint32_t(Animation.Frames.size()) + Columns - 1) / Columns : 0;
    }

    if (bAny)
    {
        constexpr size_t RgbaChannels = 4;
        FImage Sheet{ .Width = Columns * CellWidth, .Height = Rows * CellHeight };
        Sheet.Pixels.assign(size_t(Sheet.Width) * Sheet.Height * RgbaChannels, 0);
        for (size_t AnimationIndex = 0; AnimationIndex < AnimationList.size(); ++AnimationIndex)
        {
            const std::vector<Gaf::FAnimationFrame>& AnimationFrames = AnimationList[AnimationIndex].Frames;
            for (uint32_t FrameIndex = 0; FrameIndex < AnimationFrames.size(); ++FrameIndex)
            {
                const Gaf::FAnimationFrame& Frame = AnimationFrames[FrameIndex];
                if (!Visible(Frame.BitmapIndex))
                {
                    continue;
                }

                const uint32_t Cell = uint32_t(FirstCells[AnimationIndex]) + FrameIndex;
                const FPlace Place = FramePlace(Frame, Bitmaps[Frame.BitmapIndex - 1]);
                Blit(Sheet, Images[Frame.BitmapIndex - 1], Cell % Columns * CellWidth + uint32_t(Place.X - Left), Cell / Columns * CellHeight + uint32_t(Place.Y - Top),
                    Frame.bMirrorX, Frame.bMirrorY);
                bShown[Frame.BitmapIndex - 1] = true;
            }
        }

        const std::optional<std::vector<uint8_t>> Png = EncodePng(Sheet);
        if (!Png || !Extractor.Write(Entry, fs::path(Extractor.Claim(Folder, Stem, "png")).replace_extension(".png"), *Png))
        {
            Entry.Error = Entry.Error.empty() ? "cannot encode the sheet" : Entry.Error;
            return false;
        }
    }

    Json.Field("Image", bAny ? Stem + ".png" : std::string());
    Json.Field("Columns", Columns);
    Json.Field("Rows", Rows);
    Json.Field("CellWidth", CellWidth);
    Json.Field("CellHeight", CellHeight);
    Json.Field("PivotX", -Left);
    Json.Field("PivotY", -Top);
    Json.Key("Animations");
    Json.BeginArray();
    for (size_t AnimationIndex = 0; AnimationIndex < AnimationList.size(); ++AnimationIndex)
    {
        const Gaf::FAnimation& Animation = AnimationList[AnimationIndex];
        Json.BeginObject();
        Json.Field("Name", Animation.Name);
        Json.Field("FirstCell", FirstCells[AnimationIndex]);
        Json.Field("Flags", Animation.Flags);
        Json.Field("Loop", (Animation.Flags & Gaf::AnimationLoops) != 0);
        Json.Field("PingPong", (Animation.Flags & Gaf::AnimationPingPong) != 0);
        Json.Field("Reverse", (Animation.Flags & Gaf::AnimationReverse) != 0);
        Json.Field("LoopStart", Animation.LoopStart);
        Json.Key("Frames");
        Json.BeginArray();
        for (const Gaf::FAnimationFrame& Frame : Animation.Frames)
        {
            Json.BeginObject();
            Json.Field("Bitmap", Frame.BitmapIndex);
            Json.Field("OffsetX", Frame.OffsetX);
            Json.Field("OffsetY", Frame.OffsetY);
            Json.Field("Duration", Frame.Duration);
            Json.Field("DurationMs", int64_t(Frame.Duration) * AnimationPeriodMilliseconds);
            Json.Field("MirrorX", Frame.bMirrorX);
            Json.Field("MirrorY", Frame.bMirrorY);
            Json.Field("DrawFlags", Frame.DrawFlags);
            Json.Field("Blend", std::string(BlendName(Frame.DrawFlags)));
            Json.Field("EventFlags", Frame.EventFlags);
            Json.Field("EventValue", Frame.EventValue);
            if ((Frame.EventFlags & Gaf::FrameSoundEvent) != 0 && Frame.EventValue >= 1 && Frame.EventValue <= SoundFiles.size())
            {
                Json.Field("Sound", SoundFiles[Frame.EventValue - 1]);
            }

            Json.EndObject();
        }

        Json.EndArray();
        Json.EndObject();
    }

    Json.EndArray();
    Json.Key("Sounds");
    Json.BeginArray();
    for (const std::string& Name : SoundFiles)
    {
        Json.String(Name);
    }

    Json.EndArray();
    Json.Key("Fonts");
    Json.BeginArray();
    for (size_t FontIndex = 0; Fonts && FontIndex < Fonts->size(); ++FontIndex)
    {
        const Font::FFont& Font = (*Fonts)[FontIndex];
        std::vector<uint32_t> Members;
        for (const Font::FGlyph& Glyph : Font.Glyphs)
        {
            for (const uint32_t Index : { Glyph.Base.BitmapIndex, Glyph.Overlay.BitmapIndex })
            {
                if (Visible(Index) && std::ranges::find(Members, Index) == Members.end())
                {
                    Members.push_back(Index);
                    bShown[Index - 1] = true;
                }
            }
        }

        Json.BeginObject();
        const fs::path Base = Extractor.Claim(Folder, std::format("Font{}", FontIndex + 1), "fnt");
        std::vector<std::array<uint32_t, 2>> Places;
        std::string PageName;
        if (!Members.empty())
        {
            if (!WritePage(Extractor, Entry, Json, fs::path(Base).replace_extension(".png"), Images, Members, &Places))
            {
                return false;
            }

            PageName = Base.filename().string() + ".png";
        }

        // BMFont's text form, which Godot and most engines import: the base bitmap of each glyph; the overlays stay in the JSON.
        const auto PlaceOf = [&](const uint32_t Index) { return Places[size_t(std::ranges::find(Members, Index) - Members.begin())]; };
        std::string Fnt = std::format("info face=\"{} {}\" size={} bold=0 italic=0 charset=\"\" unicode=1 stretchH=100 smooth=0 aa=1 padding=0,0,0,0 spacing=1,1\n",
            Stem, FontIndex + 1, Font.LineHeight);
        std::string Chars;
        size_t CharCount = 0;
        constexpr size_t SpaceCode = 0x20;
        uint32_t ScaleWidth = 0;
        uint32_t ScaleHeight = 0;
        for (size_t Member = 0; Member < Members.size(); ++Member)
        {
            ScaleWidth = std::max(ScaleWidth, Places[Member][0] + Bitmaps[Members[Member] - 1].Width);
            ScaleHeight = std::max(ScaleHeight, Places[Member][1] + Bitmaps[Members[Member] - 1].Height);
        }

        for (size_t Index = 0; Index < Font.Glyphs.size(); ++Index)
        {
            const Font::FGlyph& Glyph = Font.Glyphs[Index];
            const uint8_t Byte = uint8_t(Index + 1);
            const bool bHasBitmap = Visible(Glyph.Base.BitmapIndex);
            if (!bHasBitmap && Byte != SpaceCode)
            {
                continue;
            }

            const uint32_t Width = bHasBitmap ? Bitmaps[Glyph.Base.BitmapIndex - 1].Width : 0;
            const uint32_t Height = bHasBitmap ? Bitmaps[Glyph.Base.BitmapIndex - 1].Height : 0;
            const std::array<uint32_t, 2> Place = bHasBitmap ? PlaceOf(Glyph.Base.BitmapIndex) : std::array<uint32_t, 2>{};
            const int32_t Advance = (Font.Flags & Font::FontProportional) != 0 && bHasBitmap ? int32_t(Width) + Font.CharSpacing : Font.Advance;
            Chars += std::format("char id={} x={} y={} width={} height={} xoffset={} yoffset={} xadvance={} page=0 chnl=15\n", uint32_t(CodePoint(Byte, Page)), Place[0],
                Place[1], Width, Height, Glyph.Base.OffsetX, Glyph.Base.OffsetY, Advance);
            ++CharCount;
        }

        Fnt += std::format("common lineHeight={} base={} scaleW={} scaleH={} pages=1 packed=0\npage id=0 file=\"{}\"\n", Font.LineHeight, Font.Baseline, ScaleWidth,
            ScaleHeight, PageName);
        Fnt += std::format("chars count={}\n", CharCount) + Chars;
        if (!Extractor.WriteText(Entry, fs::path(Base).replace_extension(".fnt"), Fnt))
        {
            return false;
        }

        Json.Field("File", Base.filename().string() + ".fnt");
        Json.Field("Flags", Font.Flags);
        Json.Field("Proportional", (Font.Flags & Font::FontProportional) != 0);
        Json.Field("LineGap", Font.LineGap);
        Json.Field("CharSpacing", Font.CharSpacing);
        Json.Field("Advance", Font.Advance);
        Json.Field("LineHeight", Font.LineHeight);
        Json.Field("Baseline", Font.Baseline);
        Json.Field("DrawMode", Font.DrawMode);
        Json.Field("Alpha", Font.Color.Alpha);
        Json.Field("Diffuse", Font.Color.Diffuse);
        Json.Field("Specular", Font.Color.Specular);
        Json.Key("Glyphs");
        Json.BeginArray();
        for (size_t Index = 0; Index < Font.Glyphs.size(); ++Index)
        {
            const Font::FGlyph& Glyph = Font.Glyphs[Index];
            const auto IsEmpty = [](const Font::FGlyphLayer& Layer) { return Layer.BitmapIndex == 0 && Layer.OffsetX == 0 && Layer.OffsetY == 0; };
            if (IsEmpty(Glyph.Base) && IsEmpty(Glyph.Overlay))
            {
                continue;
            }

            Json.BeginObject();
            Json.Field("Byte", Index + 1);
            Json.Field("Code", uint32_t(CodePoint(uint8_t(Index + 1), Page)));
            Json.Field("Bitmap", Glyph.Base.BitmapIndex);
            Json.Field("OffsetX", Glyph.Base.OffsetX);
            Json.Field("OffsetY", Glyph.Base.OffsetY);
            Json.Field("OverlayBitmap", Glyph.Overlay.BitmapIndex);
            Json.Field("OverlayOffsetX", Glyph.Overlay.OffsetX);
            Json.Field("OverlayOffsetY", Glyph.Overlay.OffsetY);
            Json.EndObject();
        }

        Json.EndArray();
        Json.EndObject();
    }

    Json.EndArray();

    // Bitmaps no animation or font shows still pack back, from their own page.
    std::vector<uint32_t> Leftovers;
    for (uint32_t Index = 1; Index <= Bitmaps.size(); ++Index)
    {
        if (!bShown[Index - 1])
        {
            Leftovers.push_back(Index);
        }
    }

    if (!Leftovers.empty())
    {
        Json.Key("Leftovers");
        Json.BeginObject();
        if (!WritePage(Extractor, Entry, Json, fs::path(Extractor.Claim(Folder, "Bitmaps", "png")).replace_extension(".png"), Images, Leftovers, nullptr, &Bitmaps))
        {
            return false;
        }

        Json.EndObject();
    }

    Json.EndObject();
    return Extractor.WriteText(Entry, Folder / (Stem + ".json"), Json.GetText());
}

// The Russian build's menu font draws Windows-1251 Cyrillic; the English one has no glyph at its capital A.
const FCodePage& DetectCodePage(FResourceArchive& Archive)
{
    constexpr size_t CyrillicA = 0xc0;
    const std::optional<std::vector<uint8_t>> File = Archive.Read("common\\data\\menu\\v-menu.gaf");
    const std::optional<std::vector<Font::FFont>> Fonts = File ? Font::LoadFonts(*File) : std::nullopt;
    const bool bCyrillic = Fonts && !Fonts->empty() && Fonts->front().Glyphs.size() >= CyrillicA && Fonts->front().Glyphs[CyrillicA - 1].Base.BitmapIndex != 0;
    return bCyrillic ? Cp1251() : Cp1252();
}

// A block of the .3DC/.3DO/.3DP grammar: its kind and name, its fields in order (repeats kept), and its child blocks.
// The grammar's first line, the format version, which the parser keeps as a bare field of the root.
bool IsFormatLine(const std::string_view Name, const std::string_view Value)
{
    return Value.empty() && !Name.empty() && std::isdigit(uint8_t(Name.front())) && Name.find('.') != std::string_view::npos;
}

void WriteBlock(FJsonWriter& Json, const FLevelPropertyBlock& Block)
{
    Json.BeginObject();
    for (const auto& [Name, Value] : Block.Fields)
    {
        if (IsFormatLine(Name, Value))
        {
            Json.Field("Format", Name);
        }
    }

    Json.Field("Kind", Block.Kind);
    Json.Field("Name", Block.Name);
    Json.Key("Fields");
    Json.BeginArray();
    for (const auto& [Name, Value] : Block.Fields)
    {
        if (IsFormatLine(Name, Value))
        {
            continue;
        }

        Json.BeginObject();
        Json.Field("Name", Name);
        Json.Field("Value", Value);
        Json.EndObject();
    }

    Json.EndArray();
    Json.Key("Children");
    Json.BeginArray();
    for (const FLevelPropertyBlock& Child : Block.Children)
    {
        WriteBlock(Json, Child);
    }

    Json.EndArray();
    Json.EndObject();
}

std::string BlockText(const FLevelPropertyBlock& Block)
{
    FJsonWriter Json;
    WriteBlock(Json, Block);
    return Json.GetText();
}

// A class library: its classes and DEFAULT-PROPERTY blocks as written, before any Derived-From is resolved.
bool ExportClasses(FExtractor& Extractor, FManifestEntry& Entry, const fs::path& Base, const std::span<const uint8_t> File, const FCodePage& Page)
{
    const std::expected<FLevelPropertyBlock, std::string> Root = ParseLevelBlocks(ToUtf8(File, Page));
    if (!Root)
    {
        Entry.Error = Root.error();
        return false;
    }

    return Extractor.WriteText(Entry, fs::path(Base).replace_extension(".json"), BlockText(*Root));
}

// The demo: each record's clock time, input bits, position and facing (DemoLoad at 004292b0).
bool ExportRecording(FExtractor& Extractor, FManifestEntry& Entry, const fs::path& Base, FResourceArchive& Archive)
{
    const std::expected<std::vector<FDemoRecord>, std::string> Records = LoadDemo(Archive);
    if (!Records)
    {
        Entry.Error = Records.error();
        return false;
    }

    FJsonWriter Json;
    Json.BeginObject();
    Json.Key("Records");
    Json.BeginArray();
    for (const FDemoRecord& Record : *Records)
    {
        Json.BeginObject();
        Json.Field("Time", Record.Time);
        Json.Field("Input", Record.Input);
        Json.Field("X", Record.X);
        Json.Field("Y", Record.Y);
        Json.Field("Direction", Record.Direction);
        Json.EndObject();
    }

    Json.EndArray();
    Json.EndObject();
    return Extractor.WriteText(Entry, fs::path(Base).replace_extension(".json"), Json.GetText());
}

std::string_view TerrainName(const ETerrain Type)
{
    constexpr std::array<std::string_view, 16> Names = { "None", "Bridge", "Corner", "Ground", "GroundLeftWall", "GroundRightWall", "HalfGround", "HalfRoof",
        "HorizontalWall", "LeftWall", "Obstacle", "RightWall", "Roof", "RoofPlus", "WallCorner", "Water" };
    return size_t(Type) < Names.size() ? Names[size_t(Type)] : "None";
}

// The JSON of the GAF an archive path names, relative to a level's folder.
std::string GafJsonPath(const std::string_view ArchiveName, const fs::path& LevelFolder)
{
    const FTarget Target = MapEntry(ArchiveName);
    return fs::path(fs::relative(Target.Folder / Target.Stem / (Target.Stem + ".json"), LevelFolder)).generic_string();
}

// One Tiled property, a string unless told otherwise.
void TiledProperty(FJsonWriter& Json, const std::string_view Name, const std::string_view Value)
{
    Json.BeginObject();
    Json.Field("name", Name);
    Json.Field("type", std::string("string"));
    Json.Field("value", Value);
    Json.EndObject();
}

void TiledNumberProperty(FJsonWriter& Json, const std::string_view Name, const int64_t Value)
{
    Json.BeginObject();
    Json.Field("name", Name);
    Json.Field("type", std::string("int"));
    Json.Field("value", Value);
    Json.EndObject();
}

// A level as a Tiled map (.tmj) with its tileset image, and its terrain descriptors and cells as JSON.
// The map's tile layers are the .3DG's MAP_ layers over its BLK_ blocks; its object layers hold the .3DO's objects with their own fields, the .3DP's patrol paths
// and the static terrain collision. The .3DO's header fields become map properties.
bool ExportLevel(FExtractor& Extractor, const std::vector<FManifestEntry*>& Entries, const fs::path& Folder, const std::string& LevelPath, FResourceArchive& Archive,
    const FCodePage& Page)
{
    FManifestEntry Shared;
    const auto Fail = [&](const std::string& Error)
    {
        for (FManifestEntry* const Entry : Entries)
        {
            Entry->Error = Error;
        }

        return false;
    };
    const std::expected<FLevel, std::string> Level = LoadLevel(Archive, LevelPath);
    if (!Level)
    {
        return Fail(Level.error());
    }

    const std::optional<std::vector<uint8_t>> ObjectText = Archive.Read(LevelPath + ".3do");
    const std::optional<std::vector<uint8_t>> PathText = Archive.Read(LevelPath + ".3dp");
    const std::expected<FLevelPropertyBlock, std::string> Objects = ObjectText ? ParseLevelBlocks(ToUtf8(*ObjectText, Page)) : std::unexpected(std::string("no .3do"));
    const std::expected<FLevelPropertyBlock, std::string> Paths = PathText ? ParseLevelBlocks(ToUtf8(*PathText, Page)) : std::unexpected(std::string("no .3dp"));
    const std::expected<FTerrainCollision, std::string> Terrain = BuildTerrainCollision(*Level);
    if (!Objects || !Paths || !Terrain)
    {
        return Fail(!Objects ? Objects.error() : !Paths ? Paths.error() : Terrain.error());
    }

    // The tileset: every block in a grid of TilesetColumns.
    constexpr uint32_t TilesetColumns = 16;
    const uint32_t TileCount = uint32_t(Level->Blocks.size());
    const uint32_t Columns = std::max(1u, std::min(TilesetColumns, TileCount));
    const uint32_t Rows = std::max(1u, (TileCount + Columns - 1) / Columns);
    constexpr size_t RgbaChannels = 4;
    FImage Tileset{ .Width = Columns * BlockWidth, .Height = Rows * BlockHeight };
    Tileset.Pixels.assign(size_t(Tileset.Width) * Tileset.Height * RgbaChannels, 0);
    for (uint32_t Index = 0; Index < TileCount; ++Index)
    {
        Blit(Tileset, ImageFromFrame(Level->Blocks[Index]), Index % Columns * BlockWidth, Index / Columns * BlockHeight);
    }

    const std::optional<std::vector<uint8_t>> TilesetPng = EncodePng(Tileset);
    if (!TilesetPng || !Extractor.Write(Shared, Folder / "Tileset.png", *TilesetPng))
    {
        return Fail("cannot write the tileset");
    }

    const std::string Id = Folder.filename().string();
    FJsonWriter Json;
    Json.BeginObject();
    Json.Field("type", std::string("map"));
    Json.Field("version", std::string("1.10"));
    Json.Field("orientation", std::string("orthogonal"));
    Json.Field("renderorder", std::string("right-down"));
    Json.Field("infinite", false);
    Json.Field("width", Level->GridWidth);
    Json.Field("height", Level->GridHeight);
    Json.Field("tilewidth", BlockWidth);
    Json.Field("tileheight", BlockHeight);
    Json.Key("properties");
    Json.BeginArray();
    for (const auto& [Name, Value] : Objects->Fields)
    {
        TiledProperty(Json, IsFormatLine(Name, Value) ? "Format" : Name, IsFormatLine(Name, Value) ? Name : Value);
    }

    TiledNumberProperty(Json, "WorldExtentX", Level->WorldExtentX);
    TiledNumberProperty(Json, "WorldExtentY", Level->WorldExtentY);
    TiledProperty(Json, "Terrain", "Terrain.json");
    Json.EndArray();

    Json.Key("tilesets");
    Json.BeginArray();
    Json.BeginObject();
    Json.Field("firstgid", 1);
    Json.Field("name", Id);
    Json.Field("image", std::string("Tileset.png"));
    Json.Field("imagewidth", Tileset.Width);
    Json.Field("imageheight", Tileset.Height);
    Json.Field("tilewidth", BlockWidth);
    Json.Field("tileheight", BlockHeight);
    Json.Field("tilecount", TileCount);
    Json.Field("columns", Columns);
    Json.Field("margin", 0);
    Json.Field("spacing", 0);
    Json.Key("tiles");
    Json.BeginArray();
    for (uint32_t Index = 0; Index < TileCount && Index < Level->BlockDepths.size(); ++Index)
    {
        Json.BeginObject();
        Json.Field("id", Index);
        Json.Key("properties");
        Json.BeginArray();
        TiledNumberProperty(Json, "Depth", Level->BlockDepths[Index]);
        Json.EndArray();
        Json.EndObject();
    }

    Json.EndArray();
    Json.EndObject();
    Json.EndArray();

    int64_t LayerId = 0;
    int64_t ObjectId = 0;
    Json.Key("layers");
    Json.BeginArray();
    const size_t LayerCells = size_t(Level->GridWidth) * Level->GridHeight;
    for (uint32_t Layer = 0; Layer < Level->LayerCount; ++Layer)
    {
        std::vector<int64_t> Data(LayerCells);
        for (size_t Cell = 0; Cell < LayerCells && Layer * LayerCells + Cell < Level->Map.size(); ++Cell)
        {
            Data[Cell] = Level->Map[Layer * LayerCells + Cell];
        }

        Json.BeginObject();
        Json.Field("type", std::string("tilelayer"));
        Json.Field("id", ++LayerId);
        Json.Field("name", std::format("Map{}", Layer + 1));
        Json.Field("x", 0);
        Json.Field("y", 0);
        Json.Field("width", Level->GridWidth);
        Json.Field("height", Level->GridHeight);
        Json.Field("opacity", 1);
        Json.Field("visible", true);
        Json.Key("data");
        Json.Numbers(Data);
        Json.EndObject();
    }

    const auto BeginGroup = [&](const std::string_view Name)
    {
        Json.BeginObject();
        Json.Field("type", std::string("objectgroup"));
        Json.Field("id", ++LayerId);
        Json.Field("name", Name);
        Json.Field("draworder", std::string("index"));
        Json.Field("x", 0);
        Json.Field("y", 0);
        Json.Field("opacity", 1);
        Json.Field("visible", true);
        Json.Key("objects");
        Json.BeginArray();
    };
    const auto EndGroup = [&]
    {
        Json.EndArray();
        Json.EndObject();
    };

    // The objects as the .3DO writes them, a point at each Position; the player's is its spawn point.
    BeginGroup("Objects");
    for (const FLevelPropertyBlock& Object : Objects->Children)
    {
        std::string ClassName(Object.Find("Class-name"));
        float X = 0.f;
        float Y = 0.f;
        if (const std::string_view Position = Object.Find("Position"); !Position.empty())
        {
            std::sscanf(std::string(Position).c_str(), "%f , %f", &X, &Y);
        }

        const auto Placed = std::ranges::find_if(Level->Objects, [&](const FLevelObject& Candidate) { return Candidate.FileIndex == size_t(ObjectId); });
        Json.BeginObject();
        Json.Field("id", ++ObjectId);
        Json.Field("name", ClassName);
        Json.Field("type", ClassName);
        Json.Field("x", X);
        Json.Field("y", Y);
        Json.Field("width", 0);
        Json.Field("height", 0);
        Json.Field("rotation", 0);
        Json.Field("visible", true);
        Json.Field("point", true);
        Json.Key("properties");
        Json.BeginArray();
        for (const auto& [Name, Value] : Object.Fields)
        {
            if (Name != "Class-name" && Name != "Position")
            {
                TiledProperty(Json, Name, Value);
            }
        }

        if (!Object.Children.empty())
        {
            FJsonWriter Blocks;
            Blocks.BeginArray();
            for (const FLevelPropertyBlock& Child : Object.Children)
            {
                WriteBlock(Blocks, Child);
            }

            Blocks.EndArray();
            TiledProperty(Json, "Blocks", Blocks.GetText());
        }

        if (Placed != Level->Objects.end() && Placed->Behaviour == "Player")
        {
            TiledProperty(Json, "Spawn", "Player");
        }

        Json.EndArray();
        Json.EndObject();
    }

    EndGroup();

    BeginGroup("PatrolPaths");
    for (const FPatrolPath& Path : Level->PatrolPaths)
    {
        if (Path.Points.empty())
        {
            continue;
        }

        Json.BeginObject();
        Json.Field("id", ++ObjectId);
        Json.Field("name", std::format("Path{}", Path.Id));
        Json.Field("x", Path.Points.front()[0]);
        Json.Field("y", Path.Points.front()[1]);
        Json.Field("rotation", 0);
        Json.Field("visible", true);
        Json.Key("polyline");
        Json.BeginArray();
        for (const std::array<int32_t, 2>& Point : Path.Points)
        {
            Json.BeginObject();
            Json.Field("x", Point[0] - Path.Points.front()[0]);
            Json.Field("y", Point[1] - Path.Points.front()[1]);
            Json.EndObject();
        }

        Json.EndArray();
        Json.Key("properties");
        Json.BeginArray();
        TiledNumberProperty(Json, "Id", Path.Id);
        Json.EndArray();
        Json.EndObject();
    }

    EndGroup();

    // The animated tiles, at their cells' top-left corners, with the GAF and animation each plays (their random start frames left out).
    BeginGroup("MapAnimations");
    FRandom Random;
    for (const FDecorAnimation& Decor : StartMapAnimations(*Level, Archive, Random))
    {
        Json.BeginObject();
        Json.Field("id", ++ObjectId);
        Json.Field("name", std::format("Animation{}", Decor.Animation + 1));
        Json.Field("x", Decor.X);
        Json.Field("y", Decor.Y);
        Json.Field("width", 0);
        Json.Field("height", 0);
        Json.Field("rotation", 0);
        Json.Field("visible", true);
        Json.Field("point", true);
        Json.Key("properties");
        Json.BeginArray();
        TiledProperty(Json, "Gaf", GafJsonPath(Decor.GafPath, Folder));
        TiledNumberProperty(Json, "Animation", int64_t(Decor.Animation) + 1);
        TiledNumberProperty(Json, "DepthBias", Decor.DepthBias);
        Json.EndArray();
        Json.EndObject();
    }

    EndGroup();

    // The static terrain collision the port builds from the .3DI, in world pixels: two-point shapes as lines, four-point ones as polygons.
    BeginGroup("Collision");
    constexpr size_t PolygonPoints = 4;
    for (const FStaticCollisionShape& Shape : Terrain->GetShapes())
    {
        const FCollisionPoint Origin = ToWorldSpace(Shape.Points[0]);
        Json.BeginObject();
        Json.Field("id", ++ObjectId);
        Json.Field("name", std::string(Shape.bWater ? "Water" : "Wall"));
        Json.Field("x", Origin.X);
        Json.Field("y", Origin.Y);
        Json.Field("rotation", 0);
        Json.Field("visible", true);
        Json.Key(Shape.PointCount == PolygonPoints ? "polygon" : "polyline");
        Json.BeginArray();
        for (size_t Index = 0; Index < Shape.PointCount; ++Index)
        {
            const FCollisionPoint Point = ToWorldSpace(Shape.Points[Index]);
            Json.BeginObject();
            Json.Field("x", Point.X - Origin.X);
            Json.Field("y", Point.Y - Origin.Y);
            Json.EndObject();
        }

        Json.EndArray();
        Json.EndObject();
    }

    EndGroup();
    Json.EndArray();
    Json.Field("nextlayerid", LayerId + 1);
    Json.Field("nextobjectid", ObjectId + 1);
    Json.EndObject();
    if (!Extractor.WriteText(Shared, Folder / (Id + ".tmj"), Json.GetText()))
    {
        return Fail("cannot write the map");
    }

    // The .3DX descriptors and the .3DI's four slots per cell, row-major, which Tiled has no place for.
    FJsonWriter Terrain3di;
    Terrain3di.BeginObject();
    Terrain3di.Field("Width", Level->GridWidth);
    Terrain3di.Field("Height", Level->GridHeight);
    Terrain3di.Field("CellWidth", BlockWidth);
    Terrain3di.Field("CellHeight", BlockHeight);
    Terrain3di.Key("Gafs");
    Terrain3di.BeginArray();
    for (size_t Index = 0; Index < Level->AnimGafs.size() && Index < Level->AnimGafSources.size(); ++Index)
    {
        Terrain3di.BeginObject();
        Terrain3di.Field("Source", Level->AnimGafSources[Index]);
        Terrain3di.Field("Json", GafJsonPath(Level->AnimGafs[Index], Folder));
        Terrain3di.EndObject();
    }

    Terrain3di.EndArray();
    // Each descriptor names a bitmap path (Gaf 0) or an animation of a 1-based Gafs entry; the folder above the bitmap or GAF gives its Type.
    Terrain3di.Key("Descriptors");
    Terrain3di.BeginArray();
    for (size_t Index = 1; Index < Level->TileTypes.size() && Index - 1 < Level->TileReferences.size(); ++Index)
    {
        const auto& [Gaf, Name] = Level->TileReferences[Index - 1];
        Terrain3di.BeginObject();
        Terrain3di.Field("Index", Index);
        Terrain3di.Field("Type", std::string(TerrainName(Level->TileTypes[Index])));
        Terrain3di.Field("Gaf", Gaf);
        Terrain3di.Field("Name", Name);
        Terrain3di.EndObject();
    }

    Terrain3di.EndArray();
    Terrain3di.Key("Slots");
    Terrain3di.BeginArray();
    constexpr size_t SlotCount = 4;
    for (size_t Slot = 0; Slot < SlotCount; ++Slot)
    {
        std::vector<int64_t> Data(LayerCells);
        for (uint32_t Y = 0; Y < Level->GridHeight; ++Y)
        {
            for (uint32_t X = 0; X < Level->GridWidth && size_t(X) * Level->GridHeight + Y < Level->Cells.size(); ++X)
            {
                Data[size_t(Y) * Level->GridWidth + X] = Level->GetCell(X, Y)[Slot];
            }
        }

        Terrain3di.Numbers(Data);
    }

    Terrain3di.EndArray();
    Terrain3di.EndObject();
    if (!Extractor.WriteText(Shared, Folder / "Terrain.json", Terrain3di.GetText()))
    {
        return Fail("cannot write the terrain");
    }

    for (FManifestEntry* const Entry : Entries)
    {
        Entry->Files = Shared.Files;
    }

    return true;
}

struct FArchiveResult
{
    std::string File;
    std::string Encoding;
    std::vector<FManifestEntry> Entries;
};

FArchiveResult ExtractArchive(FExtractor& Extractor, const fs::path& ArchivePath, const fs::path& Subfolder)
{
    FArchiveResult Result{ .File = ArchivePath.filename().string() };
    FResourceArchive Archive;
    if (!Archive.Open(ArchivePath))
    {
        Result.Entries.push_back({ .Name = Result.File, .Error = "cannot open the archive" });
        return Result;
    }

    const FCodePage& Page = DetectCodePage(Archive);
    Result.Encoding = &Page == &Cp1251() ? "windows-1251" : "windows-1252";
    std::vector<std::string> Names = Archive.GetNames();
    std::ranges::sort(Names);
    // Each level's folder and its entries, by archive path without the extension.
    std::map<std::string, std::pair<fs::path, std::vector<size_t>>> Levels;
    for (const std::string& Name : Names)
    {
        FManifestEntry Entry{ .Name = Name };
        const std::optional<std::vector<uint8_t>> File = Archive.Read(Name);
        if (!File)
        {
            Entry.Error = "cannot read the entry";
            Result.Entries.push_back(std::move(Entry));
            continue;
        }

        Entry.Size = uint32_t(File->size());
        const FTarget Target = MapEntry(Name);
        const fs::path Base = Extractor.Claim(Subfolder / Target.Folder, Target.Stem, Target.Extension);
        Entry.Kind = Target.Extension;
        if (Target.Extension == "gaf")
        {
            ExportGaf(Extractor, Entry, Base, *File, Page);
        }
        else if (Target.Extension == "tga")
        {
            const std::optional<FTgaImage> Image = DecodeTga(*File);
            const std::optional<std::vector<uint8_t>> Png = Image ? EncodePng({ .Width = Image->Width, .Height = Image->Height, .Pixels = Image->Pixels }) : std::nullopt;
            if (Png)
            {
                Extractor.Write(Entry, fs::path(Base).replace_extension(".png"), *Png);
            }
            else
            {
                Entry.Error = "cannot decode the image";
            }
        }
        else if (Target.Extension == "txt")
        {
            Extractor.WriteText(Entry, fs::path(Base).replace_extension(".txt"), ToUtf8(*File, Page));
        }
        else if (Target.Extension == "3dc")
        {
            ExportClasses(Extractor, Entry, Base, *File, Page);
        }
        else if (Target.Extension == "3dr")
        {
            ExportRecording(Extractor, Entry, Base, Archive);
        }
        else if (Target.Extension.starts_with("3d"))
        {
            // A level's five files make one map, written once all are listed.
            const std::string LevelPath = Name.substr(0, Name.rfind('.'));
            auto& Level = Levels[LevelPath];
            Level.first = Base.parent_path();
            Level.second.push_back(Result.Entries.size());
        }
        else
        {
            Extractor.Write(Entry, fs::path(Base).replace_extension("." + Target.Extension), *File);
        }

        Result.Entries.push_back(std::move(Entry));
    }

    for (const auto& [LevelPath, Level] : Levels)
    {
        std::vector<FManifestEntry*> Entries;
        for (const size_t Index : Level.second)
        {
            Entries.push_back(&Result.Entries[Index]);
        }

        ExportLevel(Extractor, Entries, Level.first, LevelPath, Archive, Page);
    }

    return Result;
}

}

int RunExtract(const std::filesystem::path& Input, const std::optional<std::filesystem::path>& Output)
{
    AttachParentConsole();
    std::error_code Error;
    const bool bFolder = fs::is_directory(Input, Error);
    const fs::path GameFolder = bFolder ? Input : Input.parent_path();
    const fs::path Root = Output.value_or(GameFolder / "Extracted");
    std::vector<std::pair<fs::path, fs::path>> Archives;
    if (bFolder)
    {
        // The slides archive's name, as VzRun opens it; its slides go under Slides.
        for (const auto& [Name, Subfolder] : { std::pair("vz-data.rsc", fs::path()), std::pair("vz-slide.rsc", fs::path()) })
        {
            for (const fs::directory_entry& Item : fs::directory_iterator(Input, Error))
            {
                std::string Lower = Item.path().filename().string();
                std::ranges::transform(Lower, Lower.begin(), [](const char Character) { return char(std::tolower(uint8_t(Character))); });
                if (Lower == Name)
                {
                    Archives.emplace_back(Item.path(), Subfolder);
                }
            }
        }
    }
    else
    {
        Archives.emplace_back(Input, fs::path());
    }

    if (Archives.empty())
    {
        std::fprintf(stderr, "No vz-data.rsc in %s\n", Input.string().c_str());
        return 1;
    }

    FExtractor Extractor(Root);
    std::vector<FArchiveResult> Results;
    size_t Failed = 0;
    std::map<std::string, size_t> Kinds;
    for (const auto& [Path, Subfolder] : Archives)
    {
        std::printf("Extracting %s\n", Path.string().c_str());
        Results.push_back(ExtractArchive(Extractor, Path, Subfolder));
        for (const FManifestEntry& Entry : Results.back().Entries)
        {
            ++Kinds[Entry.Kind];
            if (!Entry.Error.empty())
            {
                ++Failed;
                std::printf("FAIL %s: %s\n", Entry.Name.c_str(), Entry.Error.c_str());
            }
        }
    }

    // The manifest ties every archive entry to the files made from it, with their hashes, so --compress can tell what changed.
    FJsonWriter Json;
    Json.BeginObject();
    Json.Field("Version", 1);
    Json.Key("Archives");
    Json.BeginArray();
    for (const FArchiveResult& Result : Results)
    {
        Json.BeginObject();
        Json.Field("File", Result.File);
        Json.Field("Source", fs::absolute(Archives[size_t(&Result - Results.data())].first, Error).generic_string());
        Json.Field("TextEncoding", Result.Encoding);
        Json.Key("Entries");
        Json.BeginArray();
        for (const FManifestEntry& Entry : Result.Entries)
        {
            Json.BeginObject();
            Json.Field("Name", Entry.Name);
            Json.Field("Size", Entry.Size);
            if (!Entry.Error.empty())
            {
                Json.Field("Error", Entry.Error);
            }

            Json.Key("Files");
            Json.BeginArray();
            for (const FWrittenFile& File : Entry.Files)
            {
                Json.BeginObject();
                Json.Field("Path", File.Path);
                Json.Field("Hash", std::format("{:016x}", File.Hash));
                Json.EndObject();
            }

            Json.EndArray();
            Json.EndObject();
        }

        Json.EndArray();
        Json.EndObject();
    }

    Json.EndArray();
    Json.EndObject();
    {
        std::ofstream File(Root / "Manifest.json", std::ios::binary);
        File << Json.GetText();
    }

    std::string Summary;
    for (const auto& [Kind, Count] : Kinds)
    {
        Summary += std::format("{}{} {}", Summary.empty() ? "" : ", ", Count, Kind.empty() ? "other" : Kind);
    }

    std::printf("Extracted to %s: %s; %zu failed\n", Root.string().c_str(), Summary.c_str(), Failed);
    return Failed == 0 ? 0 : 1;
}

void AttachParentConsole()
{
#ifdef _WIN32
    if (_isatty(_fileno(stdout)) == 0 && GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) != FILE_TYPE_UNKNOWN)
    {
        return;
    }

    if (AttachConsole(ATTACH_PARENT_PROCESS))
    {
        FILE* Stream = nullptr;
        // Without the console the output is lost anyway, so a failed reopen changes nothing.
        static_cast<void>(freopen_s(&Stream, "CONOUT$", "w", stdout));
        static_cast<void>(freopen_s(&Stream, "CONOUT$", "w", stderr));
    }
#endif
}
