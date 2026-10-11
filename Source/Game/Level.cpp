#include "Game/Level.h"

#include "Pg/ResourceArchive.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <format>
#include <optional>
#include <unordered_set>

namespace
{
constexpr uint32_t BlockWidth = 64;
constexpr uint32_t BlockHeight = 32;
constexpr uint32_t MapHeaderSize = 24;
constexpr uint32_t MaxGridDimension = 4096;
constexpr uint32_t MaxLayers = 16;

template <typename T>
T ReadLE(const std::span<const uint8_t> Data, const size_t Offset)
{
    T Value;
    std::memcpy(&Value, Data.data() + Offset, sizeof(T));
    return Value;
}

std::string ToLower(std::string_view Text)
{
    std::string Result(Text);
    std::ranges::transform(Result, Result.begin(), [](const char C) { return C >= 'A' && C <= 'Z' ? char(C + ('a' - 'A')) : C; });
    return Result;
}

std::string_view Trim(std::string_view Text)
{
    const size_t First = Text.find_first_not_of(" \t\r");
    if (First == std::string_view::npos)
    {
        return {};
    }
    return Text.substr(First, Text.find_last_not_of(" \t\r") - First + 1);
}

// Comments run from '#' to the end of the line in every text format.
std::vector<std::string_view> SplitLines(const std::string_view Text)
{
    std::vector<std::string_view> Lines;
    size_t Start = 0;
    while (Start <= Text.size())
    {
        size_t End = Text.find('\n', Start);
        if (End == std::string_view::npos)
        {
            End = Text.size();
        }
        std::string_view Line = Text.substr(Start, End - Start);
        Line = Trim(Line.substr(0, Line.find('#')));
        if (!Line.empty())
        {
            Lines.push_back(Line);
        }
        Start = End + 1;
    }
    return Lines;
}

std::optional<int> ParseInt(const std::string_view Text)
{
    int Value = 0;
    const std::string_view Trimmed = Trim(Text);
    const auto [End, Error] = std::from_chars(Trimmed.data(), Trimmed.data() + Trimmed.size(), Value);
    if (Error != std::errc{} || End == Trimmed.data())
    {
        return std::nullopt;
    }
    return Value;
}

// Directory name holding a path's file: "...\bitmap\rightwall\gouffre002.tga" -> "rightwall".
std::string_view ParentDirectory(std::string_view Path)
{
    const size_t Last = Path.rfind('\\');
    if (Last == std::string_view::npos)
    {
        return {};
    }
    Path = Path.substr(0, Last);
    return Path.substr(Path.rfind('\\') + 1);
}

ETerrain TerrainFromName(const std::string_view Name)
{
    static constexpr std::string_view Names[] = {
        "bridge", "corner", "ground", "ground+leftwall", "ground+rightwall", "half-ground", "half-roof", "horizontalwall",
        "leftwall", "obstacle", "rightwall", "roof", "roof+", "wallcorner", "water",
    };
    const std::string Lower = ToLower(Name);
    for (size_t Index = 0; Index < std::size(Names); ++Index)
    {
        if (Lower == Names[Index])
        {
            return ETerrain(Index + 1);
        }
    }
    return ETerrain::None;
}

// .3DI slot that a semantic type occupies, from the switch in LevelLoadFiles at 00429710.
std::optional<size_t> TerrainSlot(const ETerrain Type)
{
    switch (Type)
    {
    case ETerrain::Corner:
    case ETerrain::Ground:
    case ETerrain::HalfGround:
    case ETerrain::HorizontalWall:
    case ETerrain::Water:
        return 0;
    case ETerrain::HalfRoof:
    case ETerrain::Roof:
        return 1;
    case ETerrain::Bridge:
    case ETerrain::RoofPlus:
        return 2;
    case ETerrain::GroundLeftWall:
    case ETerrain::GroundRightWall:
    case ETerrain::LeftWall:
    case ETerrain::Obstacle:
    case ETerrain::RightWall:
    case ETerrain::WallCorner:
        return 3;
    default:
        return std::nullopt;
    }
}

// Block grammar shared by .3DC/.3DO/.3DP: "KIND [", "KIND name [", "]", "field = value", "TAG [inline value]", bare directives.
struct FBlock
{
    std::string Kind;
    std::string Name;
    std::vector<std::pair<std::string, std::string>> Fields;
    std::vector<FBlock> Children;

    std::string_view Find(const std::string_view Field) const
    {
        for (const auto& [Key, Value] : Fields)
        {
            if (ToLower(Key) == ToLower(Field))
            {
                return Value;
            }
        }
        return {};
    }
};

std::expected<FBlock, std::string> ParseBlocks(const std::string_view Text)
{
    FBlock Root;
    std::vector<FBlock*> Stack{ &Root };
    for (const std::string_view Line : SplitLines(Text))
    {
        if (Line == "]")
        {
            if (Stack.size() == 1)
            {
                return std::unexpected("unbalanced ']'");
            }
            Stack.pop_back();
            continue;
        }

        const size_t Open = Line.find('[');
        if (Open != std::string_view::npos)
        {
            const std::string_view Head = Trim(Line.substr(0, Open));
            const std::string_view Rest = Trim(Line.substr(Open + 1));
            if (!Rest.empty())
            {
                if (Rest.back() != ']')
                {
                    return std::unexpected(std::format("unsupported line '{}'", Line));
                }
                Stack.back()->Fields.emplace_back(Head, Trim(Rest.substr(0, Rest.size() - 1)));
                continue;
            }
            const size_t Space = Head.find_first_of(" \t");
            FBlock Child{
                .Kind = std::string(Head.substr(0, Space)),
                .Name = Space == std::string_view::npos ? std::string() : std::string(Trim(Head.substr(Space))),
            };
            Stack.back()->Children.push_back(std::move(Child));
            Stack.push_back(&Stack.back()->Children.back());
            continue;
        }

        const size_t Equals = Line.find('=');
        if (Equals != std::string_view::npos)
        {
            Stack.back()->Fields.emplace_back(Trim(Line.substr(0, Equals)), Trim(Line.substr(Equals + 1)));
        }
        else
        {
            Stack.back()->Fields.emplace_back(Line, std::string());
        }
    }
    if (Stack.size() != 1)
    {
        return std::unexpected("unterminated block");
    }
    return Root;
}

std::expected<std::string, std::string> ReadText(FResourceArchive& Archive, const std::string& Path)
{
    const std::optional<std::vector<uint8_t>> File = Archive.Read(Path);
    if (!File)
    {
        return std::unexpected(std::format("{} missing", Path));
    }
    return std::string(File->begin(), File->end());
}

// LevelLoad3dx at 00429d80. GAF paths are absolute editor paths; the loader keeps the last three components under the zone data folder.
std::expected<void, std::string> Load3dx(FResourceArchive& Archive, const std::string& Path, const std::string_view Zone, FLevel& Level)
{
    const std::expected<std::string, std::string> Text = ReadText(Archive, Path);
    if (!Text)
    {
        return std::unexpected(Text.error());
    }
    const std::vector<std::string_view> Lines = SplitLines(*Text);
    size_t Line = 0;
    if (Lines.empty() || Lines[Line++] != "1.0")
    {
        return std::unexpected(std::format("{}: unknown .3dx file format", Path));
    }

    const std::optional<int> GafCount = Line < Lines.size() ? ParseInt(Lines[Line++]) : std::nullopt;
    if (!GafCount || *GafCount < 0 || Lines.size() - Line < size_t(*GafCount))
    {
        return std::unexpected(std::format("{}: bad GAF list", Path));
    }
    for (int Index = 0; Index < *GafCount; ++Index)
    {
        const std::string_view Source = Lines[Line++];
        size_t Cut = Source.size();
        for (int Component = 0; Component < 3 && Cut != std::string_view::npos; ++Component)
        {
            Cut = Cut == 0 ? std::string_view::npos : Source.rfind('\\', Cut - 1);
        }
        if (Cut == std::string_view::npos)
        {
            return std::unexpected(std::format("{}: GAF path '{}' too short", Path, Source));
        }
        Level.AnimGafs.push_back(std::format("{}\\data{}", Zone, Source.substr(Cut)));
    }

    const std::optional<int> TileCount = Line < Lines.size() ? ParseInt(Lines[Line++]) : std::nullopt;
    if (!TileCount || *TileCount <= 0 || Lines.size() - Line < size_t(*TileCount))
    {
        return std::unexpected(std::format("{}: no object in .3dx file", Path));
    }
    // Level3dxResolveEntry at 00429460: flag 0 names a bitmap path, any other flag is a 1-based index into the GAF list.
    Level.TileTypes.assign(1, ETerrain::None);
    for (int Index = 0; Index < *TileCount; ++Index)
    {
        const std::string_view Entry = Lines[Line++];
        const size_t Space = Entry.find_first_of(" \t");
        const std::optional<int> Flag = ParseInt(Entry.substr(0, Space));
        if (!Flag || Space == std::string_view::npos)
        {
            return std::unexpected(std::format("{}: malformed object name '{}'", Path, Entry));
        }
        std::string_view Source = Trim(Entry.substr(Space));
        if (*Flag != 0)
        {
            Source = *Flag > 0 && size_t(*Flag) <= Level.AnimGafs.size() ? std::string_view(Level.AnimGafs[*Flag - 1]) : std::string_view();
        }
        Level.TileTypes.push_back(TerrainFromName(ParentDirectory(Source)));
    }
    return {};
}

// Second half of LevelLoadFiles at 00429710.
std::expected<void, std::string> Load3di(FResourceArchive& Archive, const std::string& Path, FLevel& Level)
{
    const std::optional<std::vector<uint8_t>> File = Archive.Read(Path);
    if (!File || File->size() < 4)
    {
        return std::unexpected(std::format("{} missing or truncated", Path));
    }
    const std::span<const uint8_t> Data = *File;
    Level.WorldExtentX = ReadLE<uint16_t>(Data, 0);
    Level.WorldExtentY = ReadLE<uint16_t>(Data, 2);
    const uint32_t Width = (Level.WorldExtentX >> 6) + 1;
    const uint32_t Height = (Level.WorldExtentY >> 5) + 1;
    Level.Cells.assign(size_t{ Width } * Height, {});

    // The original does not bounds-check cell coordinates; out-of-range records are rejected here.
    for (size_t Offset = 4; Offset + 6 <= Data.size(); Offset += 6)
    {
        const uint32_t X = ReadLE<uint16_t>(Data, Offset);
        const uint32_t Y = ReadLE<uint16_t>(Data, Offset + 2);
        const int16_t TileId = ReadLE<int16_t>(Data, Offset + 4);
        if (X >= Width || Y >= Height)
        {
            return std::unexpected(std::format("{}: cell ({}, {}) outside {}x{} grid", Path, X, Y, Width, Height));
        }
        if (TileId < 0 || size_t(TileId) >= Level.TileTypes.size())
        {
            ++Level.MissingTileRefs;
            continue;
        }
        if (const std::optional<size_t> Slot = TerrainSlot(Level.TileTypes[TileId]))
        {
            Level.Cells[size_t{ X } * Height + Y][*Slot] = uint16_t(TileId);
        }
    }
    if ((Data.size() - 4) % 6 != 0)
    {
        return std::unexpected(std::format("{}: trailing bytes", Path));
    }
    return {};
}

// LevelLoad3dg at 0041eae0 loads the .3DG through the generic GAF loader (GafLoad at 00453960): BLK_ bitmaps and the MAP_ grid.
std::expected<void, std::string> Load3dg(FResourceArchive& Archive, const std::string& Path, FLevel& Level)
{
    const std::optional<std::vector<uint8_t>> File = Archive.Read(Path);
    if (!File)
    {
        return std::unexpected(std::format("{} missing", Path));
    }
    const std::optional<Gaf::FSlots> BlockSlots = Gaf::ReadEntrySlots(*File, Gaf::Tag("BLK_"));
    const std::optional<Gaf::FSlots> MapSlots = Gaf::ReadEntrySlots(*File, Gaf::Tag("MAP_"));
    if (!BlockSlots || !MapSlots)
    {
        return std::unexpected(std::format("{}: bad GAF structure", Path));
    }

    for (const std::vector<uint8_t>& Slot : *BlockSlots)
    {
        if (Slot.size() >= 4 && ReadLE<uint32_t>(Slot, 0) == Gaf::Tag("BMP_"))
        {
            std::optional<std::vector<Gaf::FFrame>> Blocks = Gaf::ParseBitmaps(Slot);
            if (!Blocks)
            {
                return std::unexpected(std::format("{}: bad BLK_ bitmaps", Path));
            }
            Level.Blocks = std::move(*Blocks);
        }
    }
    if (Level.Blocks.empty() || std::ranges::any_of(Level.Blocks, [](const Gaf::FFrame& Block) { return Block.Width != BlockWidth || Block.Height != BlockHeight; }))
    {
        return std::unexpected(std::format("{}: BLK_ blocks are not all {}x{}", Path, BlockWidth, BlockHeight));
    }

    const std::vector<uint8_t>& Header = (*MapSlots)[0];
    const std::vector<uint8_t>& Cells = (*MapSlots)[3];
    if (Header.size() != MapHeaderSize)
    {
        return std::unexpected(std::format("{}: MAP_ header is {} bytes", Path, Header.size()));
    }
    Level.LayerCount = ReadLE<uint32_t>(Header, 0);
    Level.GridWidth = ReadLE<uint32_t>(Header, 12);
    Level.GridHeight = ReadLE<uint32_t>(Header, 16);
    if (Level.LayerCount == 0 || Level.LayerCount > MaxLayers || Level.GridWidth == 0 || Level.GridWidth > MaxGridDimension ||
        Level.GridHeight == 0 || Level.GridHeight > MaxGridDimension ||
        Cells.size() != size_t{ Level.LayerCount } * Level.GridWidth * Level.GridHeight * 4)
    {
        return std::unexpected(std::format("{}: MAP_ size mismatch", Path));
    }
    Level.Map.resize(Cells.size() / 4);
    std::memcpy(Level.Map.data(), Cells.data(), Cells.size());
    if (std::ranges::any_of(Level.Map, [&](const uint32_t Block) { return Block > Level.Blocks.size(); }))
    {
        return std::unexpected(std::format("{}: MAP_ references a missing block", Path));
    }
    return {};
}

std::optional<std::pair<float, float>> ParsePosition(const std::string_view Text)
{
    const size_t Comma = Text.find(',');
    if (Comma == std::string_view::npos)
    {
        return std::nullopt;
    }
    float X = 0.f;
    float Y = 0.f;
    const std::string_view First = Trim(Text.substr(0, Comma));
    const std::string_view Second = Trim(Text.substr(Comma + 1));
    if (std::from_chars(First.data(), First.data() + First.size(), X).ec != std::errc{} ||
        std::from_chars(Second.data(), Second.data() + Second.size(), Y).ec != std::errc{})
    {
        return std::nullopt;
    }
    return std::pair{ X, Y };
}
}

std::expected<FLevel, std::string> LoadLevel(FResourceArchive& Archive, const std::string_view LevelPath)
{
    const size_t Separator = LevelPath.rfind('\\');
    if (Separator == std::string_view::npos)
    {
        return std::unexpected(std::format("level path '{}' has no zone", LevelPath));
    }
    const std::string_view Zone = LevelPath.substr(0, Separator);
    const std::string Base(LevelPath);

    FLevel Level;
    if (std::expected<void, std::string> Result = Load3dx(Archive, Base + ".3dx", Zone, Level); !Result)
    {
        return std::unexpected(Result.error());
    }
    if (std::expected<void, std::string> Result = Load3di(Archive, Base + ".3di", Level); !Result)
    {
        return std::unexpected(Result.error());
    }

    const std::expected<std::string, std::string> ObjectText = ReadText(Archive, Base + ".3do");
    if (!ObjectText)
    {
        return std::unexpected(ObjectText.error());
    }
    const std::expected<FBlock, std::string> Objects = ParseBlocks(*ObjectText);
    if (!Objects)
    {
        return std::unexpected(std::format("{}.3do: {}", Base, Objects.error()));
    }
    Level.Name = Objects->Find("LEVEL-NAME");
    Level.Id = Objects->Find("LEVEL-ID");
    for (const FBlock& Block : Objects->Children)
    {
        if (Block.Kind != "CREATE-OBJECT")
        {
            continue;
        }
        const std::optional<std::pair<float, float>> Position = ParsePosition(Block.Find("Position"));
        if (!Position)
        {
            return std::unexpected(std::format("{}.3do: object without a valid Position", Base));
        }
        Level.Objects.push_back({ .ClassName = std::string(Block.Find("Class-name")), .X = Position->first, .Y = Position->second });
    }
    if (ParseInt(Objects->Find("OBJECT-COUNT")) != int(Level.Objects.size()))
    {
        return std::unexpected(std::format("{}.3do: OBJECT-COUNT does not match {} objects", Base, Level.Objects.size()));
    }

    // Every instance must name a class from the COMMON or zone libraries, and every class parent must exist.
    std::unordered_set<std::string> Classes;
    std::vector<std::pair<std::string, std::string>> Parents;
    const std::string CommonPrefix = "common\\";
    const std::string ZonePrefix = ToLower(Zone) + "\\";
    for (const std::string& Name : Archive.GetNames())
    {
        if (!Name.ends_with(".3dc") || Name.find('\\') != Name.rfind('\\') || (!Name.starts_with(CommonPrefix) && !Name.starts_with(ZonePrefix)))
        {
            continue;
        }
        const std::expected<FBlock, std::string> Library = ReadText(Archive, Name).and_then(ParseBlocks);
        if (!Library)
        {
            return std::unexpected(std::format("{}: {}", Name, Library.error()));
        }
        for (const FBlock& Block : Library->Children)
        {
            if (Block.Kind != "OBJECT-CLASS")
            {
                continue;
            }
            const std::string ClassName = ToLower(Block.Find("Name"));
            if (!Classes.insert(ClassName).second)
            {
                return std::unexpected(std::format("{}: duplicate class '{}'", Name, ClassName));
            }
            if (const std::string_view Parent = Block.Find("Derived-From"); !Parent.empty())
            {
                Parents.emplace_back(ClassName, ToLower(Parent));
            }
        }
    }
    for (const auto& [Child, Parent] : Parents)
    {
        if (!Classes.contains(Parent))
        {
            return std::unexpected(std::format("class '{}' derives from missing '{}'", Child, Parent));
        }
    }
    for (const FLevelObject& Object : Level.Objects)
    {
        if (!Classes.contains(ToLower(Object.ClassName)))
        {
            return std::unexpected(std::format("{}.3do: unknown class '{}'", Base, Object.ClassName));
        }
    }

    // LevelGet3dgPath at 0041ebe0 prefers the .3DO 3DG-FILENAME, relative to the zone folder.
    const std::string_view GraphicsName = Objects->Find("3DG-FILENAME");
    const std::string GraphicsPath = GraphicsName.empty() ? Base + ".3dg" : std::format("{}\\{}", Zone, GraphicsName);
    if (std::expected<void, std::string> Result = Load3dg(Archive, GraphicsPath, Level); !Result)
    {
        return std::unexpected(Result.error());
    }
    if (Level.GridWidth != (Level.WorldExtentX >> 6) + 1 || Level.GridHeight != (Level.WorldExtentY >> 5) + 1)
    {
        return std::unexpected(std::format("{}: MAP_ grid {}x{} does not match .3DI extents", GraphicsPath, Level.GridWidth, Level.GridHeight));
    }
    return Level;
}
