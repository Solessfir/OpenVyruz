#include "Tools/Extract.h"

#include "Game/Animation.h"
#include "Pg/Font.h"
#include "Pg/Gaf.h"
#include "Pg/ResourceArchive.h"
#include "Pg/Sound.h"
#include "Pg/Tga.h"
#include "Tools/Json.h"
#include "Tools/Png.h"

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

// Windows-1252 and Windows-1251 from 0x80 up. The codes either leaves undefined keep their C1 control points, so a text converts back byte for byte.
constexpr std::array<char16_t, 128> Cp1252High = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
    0x00A0, 0x00A1, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7, 0x00A8, 0x00A9, 0x00AA, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF,
    0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7, 0x00B8, 0x00B9, 0x00BA, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF,
    0x00C0, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x00C7, 0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF,
    0x00D0, 0x00D1, 0x00D2, 0x00D3, 0x00D4, 0x00D5, 0x00D6, 0x00D7, 0x00D8, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x00DD, 0x00DE, 0x00DF,
    0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7, 0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00ED, 0x00EE, 0x00EF,
    0x00F0, 0x00F1, 0x00F2, 0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x00F7, 0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x00FE, 0x00FF,
};
constexpr std::array<char16_t, 128> Cp1251High = []
{
    std::array<char16_t, 128> Table = {
        0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, 0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,
        0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x0098, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,
        0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7, 0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,
        0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7, 0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,
    };
    // 0xC0 to 0xFF run straight through the Cyrillic capitals and small letters.
    constexpr size_t CyrillicStart = 0x40;
    constexpr char16_t CyrillicCapitalA = 0x0410;
    for (size_t Index = CyrillicStart; Index < Table.size(); ++Index)
    {
        Table[Index] = char16_t(CyrillicCapitalA + (Index - CyrillicStart));
    }

    return Table;
}();

using FCodePage = std::array<char16_t, 128>;

void AppendUtf8(std::string& Out, const char32_t Code)
{
    constexpr char32_t OneByte = 0x80;
    constexpr char32_t TwoBytes = 0x800;
    if (Code < OneByte)
    {
        Out += char(Code);
    }
    else if (Code < TwoBytes)
    {
        Out += char(0xC0 | Code >> 6);
        Out += char(0x80 | (Code & 0x3F));
    }
    else
    {
        Out += char(0xE0 | Code >> 12);
        Out += char(0x80 | (Code >> 6 & 0x3F));
        Out += char(0x80 | (Code & 0x3F));
    }
}

char32_t CodePoint(const uint8_t Byte, const FCodePage& Page)
{
    constexpr uint8_t HighStart = 0x80;
    return Byte < HighStart ? char32_t(Byte) : char32_t(Page[Byte - HighStart]);
}

std::string ToUtf8(const std::span<const uint8_t> Bytes, const FCodePage& Page)
{
    std::string Out;
    Out.reserve(Bytes.size());
    for (const uint8_t Byte : Bytes)
    {
        AppendUtf8(Out, CodePoint(Byte, Page));
    }

    return Out;
}

std::span<const uint8_t> AsBytes(const std::string_view Text)
{
    return { reinterpret_cast<const uint8_t*>(Text.data()), Text.size() };
}

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

uint64_t Fnv1a(const std::span<const uint8_t> Bytes)
{
    constexpr uint64_t Offset = 0xcbf29ce484222325;
    constexpr uint64_t Prime = 0x100000001b3;
    uint64_t Hash = Offset;
    for (const uint8_t Byte : Bytes)
    {
        Hash = (Hash ^ Byte) * Prime;
    }

    return Hash;
}

// 16-bit PCM RIFF WAVE from the decoder's floats.
std::vector<uint8_t> EncodeWav(const Sound::FSample& Sample)
{
    constexpr uint16_t PcmFormat = 1;
    constexpr uint16_t BitsPerSample = 16;
    constexpr uint32_t FormatChunkSize = 16;
    constexpr uint32_t HeaderSize = 36;
    constexpr float Peak = 32767.f;
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
        U16(uint16_t(int16_t(std::lround(std::clamp(Value, -1.f, 1.f) * Peak))));
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

        Entry.Files.push_back({ .Path = Relative.generic_string(), .Hash = Fnv1a(Bytes) });
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

// A GAF as its atlas, its animations and fonts in JSON, BMFont files for the fonts, and its samples as WAV.
bool ExportGaf(FExtractor& Extractor, FManifestEntry& Entry, const fs::path& Base, const std::span<const uint8_t> File, const FCodePage& Page)
{
    const std::optional<std::vector<Gaf::FFrame>> Frames = Gaf::LoadFrames(File);
    const std::vector<Gaf::FFrame> Bitmaps = Frames.value_or(std::vector<Gaf::FFrame>{});
    const std::optional<std::vector<Gaf::FAnimation>> Animations = Gaf::LoadAnimations(File, Bitmaps.size());
    const std::optional<std::vector<Sound::FSample>> Samples = Sound::DecodeGafSamples(File);
    const std::optional<std::vector<Font::FFont>> Fonts = Font::LoadFonts(File);
    const std::string Stem = Base.filename().string();

    std::vector<FImage> Images;
    Images.reserve(Bitmaps.size());
    for (const Gaf::FFrame& Bitmap : Bitmaps)
    {
        Images.push_back(ImageFromFrame(Bitmap));
    }

    const FAtlas Atlas = PackAtlas(Images);
    if (!Images.empty())
    {
        const std::optional<std::vector<uint8_t>> Png = EncodePng(Atlas.Image);
        if (!Png || !Extractor.Write(Entry, fs::path(Base).replace_extension(".png"), *Png))
        {
            Entry.Error = Entry.Error.empty() ? "cannot encode the atlas" : Entry.Error;
            return false;
        }
    }

    std::vector<std::string> SoundFiles;
    for (size_t Index = 0; Samples && Index < Samples->size(); ++Index)
    {
        const std::string Name = std::format("{}Sound{}.wav", Stem, Index + 1);
        if (!Extractor.Write(Entry, Base.parent_path() / Name, EncodeWav((*Samples)[Index])))
        {
            return false;
        }

        SoundFiles.push_back(Name);
    }

    FJsonWriter Json;
    Json.BeginObject();
    Json.Field("Image", Images.empty() ? std::string() : Stem + ".png");
    Json.Field("FrameMilliseconds", AnimationPeriodMilliseconds);
    Json.Key("Bitmaps");
    Json.BeginArray();
    for (size_t Index = 0; Index < Bitmaps.size(); ++Index)
    {
        Json.BeginObject();
        Json.Field("X", Atlas.Places[Index][0]);
        Json.Field("Y", Atlas.Places[Index][1]);
        Json.Field("Width", Bitmaps[Index].Width);
        Json.Field("Height", Bitmaps[Index].Height);
        Json.Field("TrimX", Bitmaps[Index].TrimX);
        Json.Field("TrimY", Bitmaps[Index].TrimY);
        Json.EndObject();
    }

    Json.EndArray();
    Json.Key("Animations");
    Json.BeginArray();
    constexpr uint32_t LoopFlag = 2;
    constexpr uint32_t PingPongFlag = 0x20;
    constexpr uint32_t ReverseFlag = 0x40;
    constexpr uint32_t SoundEventFlag = 1;
    for (const Gaf::FAnimation& Animation : Animations.value_or(std::vector<Gaf::FAnimation>{}))
    {
        Json.BeginObject();
        Json.Field("Name", Animation.Name);
        Json.Field("Flags", Animation.Flags);
        Json.Field("Loop", (Animation.Flags & LoopFlag) != 0);
        Json.Field("PingPong", (Animation.Flags & PingPongFlag) != 0);
        Json.Field("Reverse", (Animation.Flags & ReverseFlag) != 0);
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
            if ((Frame.EventFlags & SoundEventFlag) != 0 && Frame.EventValue >= 1 && Frame.EventValue <= SoundFiles.size())
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
        const std::string FontFile = std::format("{}Font{}.fnt", Stem, FontIndex + 1);
        // BMFont's text form, which Godot and most engines import: the base bitmap of each glyph; the overlays stay in the JSON.
        std::string Fnt = std::format("info face=\"{} {}\" size={} bold=0 italic=0 charset=\"\" unicode=1 stretchH=100 smooth=0 aa=1 padding=0,0,0,0 spacing=1,1\n",
            Stem, FontIndex + 1, Font.LineHeight);
        Fnt += std::format("common lineHeight={} base={} scaleW={} scaleH={} pages=1 packed=0\npage id=0 file=\"{}.png\"\n", Font.LineHeight, Font.Baseline,
            Atlas.Image.Width, Atlas.Image.Height, Stem);
        std::string Chars;
        size_t CharCount = 0;
        constexpr size_t SpaceCode = 0x20;
        for (size_t Index = 0; Index < Font.Glyphs.size(); ++Index)
        {
            const Font::FGlyph& Glyph = Font.Glyphs[Index];
            const uint8_t Byte = uint8_t(Index + 1);
            const bool bHasBitmap = Glyph.Base.BitmapIndex >= 1 && Glyph.Base.BitmapIndex <= Bitmaps.size();
            if (!bHasBitmap && Byte != SpaceCode)
            {
                continue;
            }

            const uint32_t Width = bHasBitmap ? Bitmaps[Glyph.Base.BitmapIndex - 1].Width : 0;
            const uint32_t Height = bHasBitmap ? Bitmaps[Glyph.Base.BitmapIndex - 1].Height : 0;
            const std::array<uint32_t, 2> Place = bHasBitmap ? Atlas.Places[Glyph.Base.BitmapIndex - 1] : std::array<uint32_t, 2>{};
            const int32_t Advance = (Font.Flags & Font::FontProportional) != 0 && bHasBitmap ? int32_t(Width) + Font.CharSpacing : Font.Advance;
            Chars += std::format("char id={} x={} y={} width={} height={} xoffset={} yoffset={} xadvance={} page=0 chnl=15\n", uint32_t(CodePoint(Byte, Page)), Place[0],
                Place[1], Width, Height, Glyph.Base.OffsetX, Glyph.Base.OffsetY, Advance);
            ++CharCount;
        }

        Fnt += std::format("chars count={}\n", CharCount) + Chars;
        if (!Extractor.WriteText(Entry, Base.parent_path() / FontFile, Fnt))
        {
            return false;
        }

        Json.BeginObject();
        Json.Field("File", FontFile);
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
            if (Glyph.Base.BitmapIndex == 0 && Glyph.Overlay.BitmapIndex == 0)
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
    Json.EndObject();
    return Extractor.WriteText(Entry, fs::path(Base).replace_extension(".json"), Json.GetText());
}

// The Russian build's menu font draws Windows-1251 Cyrillic; the English one has no glyph at its capital A.
const FCodePage& DetectCodePage(FResourceArchive& Archive)
{
    constexpr size_t CyrillicA = 0xc0;
    const std::optional<std::vector<uint8_t>> File = Archive.Read("common\\data\\menu\\v-menu.gaf");
    const std::optional<std::vector<Font::FFont>> Fonts = File ? Font::LoadFonts(*File) : std::nullopt;
    const bool bCyrillic = Fonts && !Fonts->empty() && Fonts->front().Glyphs.size() >= CyrillicA && Fonts->front().Glyphs[CyrillicA - 1].Base.BitmapIndex != 0;
    return bCyrillic ? Cp1251High : Cp1252High;
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
    Result.Encoding = &Page == &Cp1251High ? "windows-1251" : "windows-1252";
    std::vector<std::string> Names = Archive.GetNames();
    std::ranges::sort(Names);
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
        else
        {
            // ponytail: the level files, class libraries and the demo stay in their original form until their converters land.
            Extractor.Write(Entry, fs::path(Base).replace_extension("." + Target.Extension), *File);
        }

        Result.Entries.push_back(std::move(Entry));
    }

    return Result;
}

#ifdef _WIN32
// The game is a windowed program, so a console that started it sees nothing unless the output is attached to it.
void AttachParentConsole()
{
    if (_isatty(_fileno(stdout)) == 0 && GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) != FILE_TYPE_UNKNOWN)
    {
        return;
    }

    if (AttachConsole(ATTACH_PARENT_PROCESS))
    {
        FILE* Stream = nullptr;
        freopen_s(&Stream, "CONOUT$", "w", stdout);
        freopen_s(&Stream, "CONOUT$", "w", stderr);
    }
}
#endif
}

int RunExtract(const std::filesystem::path& Input, const std::optional<std::filesystem::path>& Output)
{
#ifdef _WIN32
    AttachParentConsole();
#endif
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
