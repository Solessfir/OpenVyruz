#include "Tools/Compress.h"

#include "Game/Level.h"
#include "Pg/ResourceArchive.h"
#include "Pg/Tga.h"
#include "Tools/Extract.h"
#include "Tools/Json.h"
#include "Tools/Png.h"
#include "Tools/Text.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
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

std::optional<std::vector<uint8_t>> ReadFile(const fs::path& Path)
{
    std::ifstream File(Path, std::ios::binary);
    if (!File)
    {
        return std::nullopt;
    }

    return std::vector<uint8_t>(std::istreambuf_iterator<char>(File), std::istreambuf_iterator<char>());
}

bool WriteFile(const fs::path& Path, const std::span<const uint8_t> Bytes)
{
    std::ofstream File(Path, std::ios::binary);
    File.write(reinterpret_cast<const char*>(Bytes.data()), std::streamsize(Bytes.size()));
    return bool(File);
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

// A PNG back to an uncompressed 24-bit Targa, rows in the order the original file kept them.
std::optional<std::vector<uint8_t>> TgaFromPng(const std::span<const uint8_t> Png, const std::span<const uint8_t> Original)
{
    constexpr size_t HeaderSize = 18;
    constexpr size_t DescriptorOffset = 17;
    constexpr uint8_t TopToBottomFlag = 0x20;
    constexpr uint8_t UncompressedTrueColor = 2;
    constexpr uint8_t RgbBits = 24;
    constexpr size_t RgbaChannels = 4;
    const std::optional<FImage> Image = DecodePng(Png);
    if (!Image || Image->Width > UINT16_MAX || Image->Height > UINT16_MAX)
    {
        return std::nullopt;
    }

    const bool bTopDown = Original.size() > DescriptorOffset && (Original[DescriptorOffset] & TopToBottomFlag) != 0;
    std::vector<uint8_t> Out(HeaderSize, 0);
    Out[2] = UncompressedTrueColor;
    Out[12] = uint8_t(Image->Width);
    Out[13] = uint8_t(Image->Width >> 8);
    Out[14] = uint8_t(Image->Height);
    Out[15] = uint8_t(Image->Height >> 8);
    Out[16] = RgbBits;
    Out[DescriptorOffset] = bTopDown ? TopToBottomFlag : 0;
    for (uint32_t Row = 0; Row < Image->Height; ++Row)
    {
        const uint32_t Source = bTopDown ? Row : Image->Height - 1 - Row;
        for (uint32_t Column = 0; Column < Image->Width; ++Column)
        {
            const uint8_t* const Pixel = &Image->Pixels[(size_t(Source) * Image->Width + Column) * RgbaChannels];
            Out.push_back(Pixel[2]);
            Out.push_back(Pixel[1]);
            Out.push_back(Pixel[0]);
        }
    }

    return Out;
}

// A block back in the .3DC/.3DO/.3DP grammar: the root's fields as "NAME [value]" lines, every other block's as "Name = value", tabs for depth.
void AppendBlock(std::string& Out, const FJsonValue& Block, const size_t Depth)
{
    const std::string Indent(Depth == 0 ? 0 : Depth - 1, '\t');
    constexpr std::string_view LineEnd = "\r\n";
    if (Depth > 0)
    {
        const std::string_view Name = Block["Name"].AsString();
        Out += std::format("{}{}{}{} [{}", Indent, Block["Kind"].AsString(), Name.empty() ? "" : " ", Name, LineEnd);
    }

    for (const FJsonValue& Field : Block["Fields"].Elements)
    {
        const std::string_view Name = Field["Name"].AsString();
        const std::string_view Value = Field["Value"].AsString();
        if (Depth == 0)
        {
            Out += Value.empty() ? std::format("{}{}", Name, LineEnd) : std::format("{} [{}]{}", Name, Value, LineEnd);
        }
        else
        {
            Out += Value.empty() ? std::format("{}\t{}{}", Indent, Name, LineEnd) : std::format("{}\t{}\t= {}{}", Indent, Name, Value, LineEnd);
        }
    }

    for (const FJsonValue& Child : Block["Children"].Elements)
    {
        Out += Depth == 0 ? LineEnd : "";
        AppendBlock(Out, Child, Depth + 1);
    }

    if (Depth > 0)
    {
        Out += std::format("{}]{}", Indent, LineEnd);
    }
}

std::optional<std::string> BlocksText(const std::span<const uint8_t> Json)
{
    const std::optional<FJsonValue> Root = ParseJson(std::string_view(reinterpret_cast<const char*>(Json.data()), Json.size()));
    if (!Root || Root->Type != FJsonValue::EType::Object)
    {
        return std::nullopt;
    }

    std::string Out = std::format("{}\r\n", (*Root)["Format"].AsString().empty() ? std::string_view("1.3") : (*Root)["Format"].AsString());
    AppendBlock(Out, *Root, 0);
    return Out;
}

struct FPackResult
{
    size_t Unchanged = 0;
    size_t Rebuilt = 0;
    size_t Failed = 0;
};

// One archive of the manifest, packed into Output.
FPackResult PackArchive(const fs::path& Extracted, const FJsonValue& Archive, const fs::path& Output)
{
    FPackResult Result;
    FResourceArchive Source;
    const fs::path SourcePath(std::u8string(reinterpret_cast<const char8_t*>(Archive["Source"].AsString().data()), Archive["Source"].AsString().size()));
    if (!Source.Open(SourcePath))
    {
        std::printf("FAIL cannot open the source archive %s\n", SourcePath.string().c_str());
        ++Result.Failed;
        return Result;
    }

    const FCodePage& Page = CodePageNamed(Archive["TextEncoding"].AsString());
    std::vector<std::pair<std::string, std::vector<uint8_t>>> Files;
    for (const std::string& Name : Source.GetStoredNames())
    {
        std::string Key = Lower(Name);
        std::ranges::replace(Key, '/', '\\');
        const auto Entry = std::ranges::find_if(Archive["Entries"].Elements, [&](const FJsonValue& Candidate) { return Candidate["Name"].AsString() == Key; });
        std::optional<std::vector<uint8_t>> Original = Source.Read(Name);
        if (!Original)
        {
            std::printf("FAIL cannot read %s from the source archive\n", Name.c_str());
            ++Result.Failed;
            continue;
        }

        // An entry is edited when any file made from it is missing or no longer hashes the same.
        bool bEdited = false;
        std::vector<std::pair<fs::path, std::vector<uint8_t>>> Made;
        for (const FJsonValue& File : Entry == Archive["Entries"].Elements.end() ? std::vector<FJsonValue>{} : (*Entry)["Files"].Elements)
        {
            const fs::path Path = Extracted / fs::path(std::u8string(reinterpret_cast<const char8_t*>(File["Path"].AsString().data()), File["Path"].AsString().size()));
            std::optional<std::vector<uint8_t>> Bytes = ReadFile(Path);
            bEdited = bEdited || !Bytes || std::format("{:016x}", HashBytes(*Bytes)) != File["Hash"].AsString();
            Made.emplace_back(Path, Bytes.value_or(std::vector<uint8_t>{}));
        }

        if (!bEdited)
        {
            Files.emplace_back(Name, std::move(*Original));
            ++Result.Unchanged;
            continue;
        }

        const std::string Kind = Extension(Name);
        std::optional<std::vector<uint8_t>> Rebuilt;
        if (Made.size() == 1 && Kind == "wav")
        {
            Rebuilt = Made.front().second;
        }
        else if (Made.size() == 1 && Kind == "txt")
        {
            Rebuilt = FromUtf8(std::string_view(reinterpret_cast<const char*>(Made.front().second.data()), Made.front().second.size()), Page);
        }
        else if (Made.size() == 1 && Kind == "tga")
        {
            Rebuilt = TgaFromPng(Made.front().second, *Original);
        }
        else if (Made.size() == 1 && Kind == "3dc")
        {
            const std::optional<std::string> Text = BlocksText(Made.front().second);
            Rebuilt = Text ? FromUtf8(*Text, Page) : std::nullopt;
        }
        else
        {
            std::printf("FAIL %s: editing .%s files is not supported yet; restore %s\n", Name.c_str(), Kind.c_str(),
                Made.empty() ? "it" : Made.front().first.filename().string().c_str());
            ++Result.Failed;
            continue;
        }

        if (!Rebuilt)
        {
            std::printf("FAIL %s: %s cannot be converted back\n", Name.c_str(), Made.front().first.string().c_str());
            ++Result.Failed;
            continue;
        }

        std::printf("Rebuilt %s\n", Name.c_str());
        Files.emplace_back(Name, std::move(*Rebuilt));
        ++Result.Rebuilt;
    }

    std::error_code Error;
    fs::create_directories(Output, Error);
    const fs::path Target = Output / fs::path(std::u8string(reinterpret_cast<const char8_t*>(Archive["File"].AsString().data()), Archive["File"].AsString().size()));
    if (Result.Failed == 0 && !WriteResourceArchive(Target, Files))
    {
        std::printf("FAIL cannot write %s\n", Target.string().c_str());
        ++Result.Failed;
    }

    return Result;
}
}

int RunCompress(const std::filesystem::path& Extracted, const std::optional<std::filesystem::path>& Output)
{
    AttachParentConsole();
    const std::optional<std::vector<uint8_t>> ManifestBytes = ReadFile(Extracted / "Manifest.json");
    const std::optional<FJsonValue> Manifest =
        ManifestBytes ? ParseJson(std::string_view(reinterpret_cast<const char*>(ManifestBytes->data()), ManifestBytes->size())) : std::nullopt;
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
        const FPackResult Result = PackArchive(Extracted, Archive, Target);
        Total.Unchanged += Result.Unchanged;
        Total.Rebuilt += Result.Rebuilt;
        Total.Failed += Result.Failed;
    }

    std::printf("Packed to %s: %zu unchanged, %zu rebuilt, %zu failed\n", Target.string().c_str(), Total.Unchanged, Total.Rebuilt, Total.Failed);
    return Total.Failed == 0 ? 0 : 1;
}

int VerifyExtractRoundTrip(const std::filesystem::path& GameFolder)
{
    const fs::path Work = fs::temp_directory_path() / "OpenVyruzRoundTrip";
    std::error_code Error;
    fs::remove_all(Work, Error);
    const fs::path Extracted = Work / "Extracted";
    const fs::path Packed = Work / "Packed";
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

    // Each class library's JSON written back as text parses to the same blocks as the original.
    FResourceArchive Data;
    Data.Open(GameFolder / "vz-data.rsc");
    const std::optional<std::vector<uint8_t>> ManifestBytes = ReadFile(Extracted / "Manifest.json");
    const std::optional<FJsonValue> Manifest =
        ManifestBytes ? ParseJson(std::string_view(reinterpret_cast<const char*>(ManifestBytes->data()), ManifestBytes->size())) : std::nullopt;
    size_t Libraries = 0;
    for (const FJsonValue& Entry : Manifest ? (*Manifest)["Archives"][0]["Entries"].Elements : std::vector<FJsonValue>{})
    {
        const std::string Name(Entry["Name"].AsString());
        if (Extension(Name) != "3dc")
        {
            continue;
        }

        const std::optional<std::vector<uint8_t>> Original = Data.Read(Name);
        const std::string_view JsonPath = Entry["Files"][0]["Path"].AsString();
        const std::optional<std::vector<uint8_t>> Json = ReadFile(Extracted / fs::path(std::u8string(reinterpret_cast<const char8_t*>(JsonPath.data()), JsonPath.size())));
        const std::optional<std::string> Text = Json ? BlocksText(*Json) : std::nullopt;
        const auto Same = [](const auto& Self, const FLevelPropertyBlock& A, const FLevelPropertyBlock& B) -> bool
        {
            return A.Kind == B.Kind && A.Name == B.Name && A.Fields == B.Fields && A.Children.size() == B.Children.size() &&
                   std::ranges::equal(A.Children, B.Children, [&](const FLevelPropertyBlock& X, const FLevelPropertyBlock& Y) { return Self(Self, X, Y); });
        };
        const std::expected<FLevelPropertyBlock, std::string> Before = Original ? ParseLevelBlocks(ToUtf8(*Original, Cp1252())) : std::unexpected(std::string());
        const std::expected<FLevelPropertyBlock, std::string> After = Text ? ParseLevelBlocks(*Text) : std::unexpected(std::string());
        if (!Before || !After || !Same(Same, *Before, *After))
        {
            std::printf("Round trip: FAIL, %s does not come back from its JSON\n", Name.c_str());
            return 1;
        }

        ++Libraries;
    }

    // An edited text and an edited image come back through their converters.
    const fs::path TextPath = Extracted / "Text" / "Start.txt";
    const fs::path ImagePath = Extracted / "Common" / "Extra" / "Story.png";
    std::optional<std::vector<uint8_t>> TextBytes = ReadFile(TextPath);
    const std::optional<std::vector<uint8_t>> ImageBytes = ReadFile(ImagePath);
    std::optional<FImage> Image = ImageBytes ? DecodePng(*ImageBytes) : std::nullopt;
    if (!TextBytes || !Image)
    {
        std::printf("Round trip: FAIL, the files to edit are missing\n");
        return 1;
    }

    constexpr std::string_view Added = " - edited";
    TextBytes->insert(TextBytes->end(), Added.begin(), Added.end());
    Image->Pixels[0] = uint8_t(~Image->Pixels[0]);
    const std::optional<std::vector<uint8_t>> EditedPng = EncodePng(*Image);
    if (!WriteFile(TextPath, *TextBytes) || !EditedPng || !WriteFile(ImagePath, *EditedPng) || RunCompress(Extracted, Packed) != 0)
    {
        std::printf("Round trip: FAIL, the edited tree did not pack\n");
        return 1;
    }

    FResourceArchive Edited;
    Edited.Open(Packed / "vz-data.rsc");
    const std::optional<std::vector<uint8_t>> PackedText = Edited.Read("start.txt");
    const std::optional<std::vector<uint8_t>> PackedImage = Edited.Read("common\\data\\extra\\story.tga");
    const std::optional<FTgaImage> Decoded = PackedImage ? DecodeTga(*PackedImage) : std::nullopt;
    const std::optional<std::vector<uint8_t>> OriginalText = Data.Read("start.txt");
    if (!PackedText || !OriginalText || std::string_view(reinterpret_cast<const char*>(PackedText->data()), PackedText->size()) !=
                                             std::string(reinterpret_cast<const char*>(OriginalText->data()), OriginalText->size()) + std::string(Added) ||
        !Decoded || Decoded->Pixels != Image->Pixels)
    {
        std::printf("Round trip: FAIL, the edited text or image did not come back\n");
        return 1;
    }

    fs::remove_all(Work, Error);
    std::printf("Round trip: %zu entries read back unchanged, %zu class libraries come back from JSON, an edited text and image packed\n", Compared, Libraries);
    return 0;
}
