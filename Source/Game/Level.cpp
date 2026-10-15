#include "Game/Level.h"

#include "Pg/Binary.h"
#include "Pg/ResourceArchive.h"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <optional>
#include <ranges>
#include <unordered_map>

namespace
{
constexpr uint32_t MaxGridDimension = 4096;
constexpr uint32_t MaxLayers = 16;
constexpr size_t MapHeaderSlot = 0;
constexpr size_t MapCellSlot = 3;
constexpr size_t BlockMetadataSlot = 0;
constexpr size_t MaxPropertyDepth = 64;
constexpr size_t MaxClassDepth = 256;
constexpr int MaxObjectAnimationIndex = std::numeric_limits<int>::max() - ObjectDirectionCount;

// MAP_ slot 0.
struct FMapHeader
{
    uint32_t LayerCount;
    // Original render-cache size; not a map scale.
    uint32_t CacheWidth;
    uint32_t CacheHeight;
    uint32_t GridWidth;
    uint32_t GridHeight;
    uint32_t Reserved;
};
static_assert(sizeof(FMapHeader) == 24);

// BLK_ slot 0; GafParseSlot0 at 0047b830 copies the final word to the runtime block's depth.
struct FBlockMetadata
{
    uint32_t Flags;
    int32_t OffsetX;
    int32_t OffsetY;
    int32_t Depth;
};
static_assert(sizeof(FBlockMetadata) == 16);

struct F3diHeader
{
    uint16_t WorldExtentX;
    uint16_t WorldExtentY;
};
static_assert(sizeof(F3diHeader) == 4);

struct F3diRecord
{
    uint16_t X;
    uint16_t Y;
    int16_t TileId;
};
static_assert(sizeof(F3diRecord) == 6);

// Grid size covering a world extent, as computed in LevelLoadFiles at 00429710.
constexpr uint32_t GridWidthFromExtent(const uint32_t ExtentX)
{
    return ExtentX / BlockWidth + 1;
}

constexpr uint32_t GridHeightFromExtent(const uint32_t ExtentY)
{
    return ExtentY / BlockHeight + 1;
}
static_assert(GridWidthFromExtent(5056) == 80 && GridHeightFromExtent(4128) == 130);

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
    if (Trimmed.empty())
    {
        return std::nullopt;
    }

    const auto [End, Error] = std::from_chars(Trimmed.data(), Trimmed.data() + Trimmed.size(), Value);
    if (Error != std::errc{} || End != Trimmed.data() + Trimmed.size())
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
using FBlock = FLevelPropertyBlock;

std::expected<FBlock, std::string> ParseBlocks(const std::string_view Text)
{
    FBlock Root;
    std::vector<FBlock*> Stack{ &Root };
    const std::vector<std::string_view> Lines = SplitLines(Text);
    if (Lines.empty() || Lines.front() != "1.3")
    {
        return std::unexpected("unknown block file format");
    }

    for (const std::string_view Line : Lines)
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
            if (Stack.size() >= MaxPropertyDepth)
            {
                return std::unexpected("property nesting limit exceeded");
            }

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

bool IsAttribute(const FBlock& Block)
{
    const std::string Kind = ToLower(Block.Kind);
    return Kind == "type" || Kind == "add" || Kind == "create";
}

// LoadObjectClassLibrary at 0040a010 appends attributes; LevelLoad3do at 00413d80 overrides each inherited attribute once.
void MergeProperties(FBlock& Target, const FBlock& Source, const bool bInstance = false)
{
    for (const auto& [Key, Value] : Source.Fields)
    {
        const std::string LowerKey = ToLower(Key);
        if (LowerKey == "override-attributes")
        {
            std::erase_if(Target.Children, IsAttribute);
        }
        else if (LowerKey == "override-objects" || LowerKey == "override-random-objects")
        {
            const std::string_view Directive = LowerKey == "override-objects" ? "have" : "random";
            std::erase_if(Target.Fields, [&](const auto& Field) { return ToLower(Field.first.substr(0, Field.first.find_first_of(" \t"))) == Directive; });
        }
    }

    std::erase_if(Target.Fields, [&](const auto& Field)
    {
        const std::string Key = ToLower(Field.first.substr(0, Field.first.find_first_of(" \t")));
        if (Key == "have" || Key == "random")
        {
            return false;
        }

        return std::ranges::any_of(Source.Fields, [&](const auto& Override) { return ToLower(Field.first) == ToLower(Override.first); });
    });
    Target.Fields.insert(Target.Fields.end(), Source.Fields.begin(), Source.Fields.end());

    const size_t InheritedCount = Target.Children.size();
    std::vector<bool> Used(InheritedCount);
    for (const FBlock& Child : Source.Children)
    {
        const bool bAttribute = IsAttribute(Child);
        size_t Match = InheritedCount;
        if (!bAttribute || bInstance)
        {
            for (size_t Index = InheritedCount; Index > 0; --Index)
            {
                const size_t Candidate = Index - 1;
                // An ADD Ammo stock overrides the stock of the same ammo (the loader looks the ammo up among the object's stocks), and a session's TYPE Generator the one of the same product.
                const std::string_view KeyField = ToLower(Child.Kind) == "add" && ToLower(Child.Name) == "ammo"       ? "Name"
                                                  : ToLower(Child.Kind) == "type" && ToLower(Child.Name) == "generator" ? "Product"
                                                                                                                         : "";
                if (!Used[Candidate] && ToLower(Target.Children[Candidate].Kind) == ToLower(Child.Kind) &&
                    ToLower(Target.Children[Candidate].Name) == ToLower(Child.Name) &&
                    (KeyField.empty() || ToLower(Target.Children[Candidate].Find(KeyField)) == ToLower(Child.Find(KeyField))))
                {
                    Match = Candidate;
                    break;
                }
            }
        }

        if (Match != InheritedCount)
        {
            Used[Match] = true;
            MergeProperties(Target.Children[Match], Child, bInstance);
        }
        else
        {
            Target.Children.push_back(Child);
        }
    }
}

enum class EClassResolution : uint8_t { Unresolved, Resolving, Resolved };

struct FClassDefinition
{
    FBlock Defaults;
    FBlock Declared;
    FBlock Resolved;
    EClassResolution State = EClassResolution::Unresolved;
};

using FClassLibrary = std::unordered_map<std::string, FClassDefinition>;

std::expected<void, std::string> ResolveClass(FClassLibrary& Classes, const std::string& Name, const size_t Depth = 0)
{
    const auto Found = Classes.find(Name);
    if (Found == Classes.end())
    {
        return std::unexpected(std::format("missing class '{}'", Name));
    }

    FClassDefinition& Class = Found->second;
    if (Class.State == EClassResolution::Resolved)
    {
        return {};
    }

    if (Class.State == EClassResolution::Resolving)
    {
        return std::unexpected(std::format("class inheritance cycle at '{}'", Name));
    }

    if (Depth >= MaxClassDepth)
    {
        return std::unexpected(std::format("class inheritance limit at '{}'", Name));
    }

    Class.State = EClassResolution::Resolving;
    Class.Resolved = Class.Defaults;
    if (const std::string_view Parent = Class.Declared.Find("Derived-From"); !Parent.empty())
    {
        const std::string ParentName = ToLower(Parent);
        if (std::expected<void, std::string> Result = ResolveClass(Classes, ParentName, Depth + 1); !Result)
        {
            return std::unexpected(std::format("class '{}': {}", Name, Result.error()));
        }

        // Derived-From copies the entire parent record, replacing even the child's library defaults.
        Class.Resolved = Classes.at(ParentName).Resolved;
    }

    MergeProperties(Class.Resolved, Class.Declared);
    Class.Resolved.Kind = Class.Declared.Kind;
    Class.Resolved.Name = Class.Declared.Name;
    Class.State = EClassResolution::Resolved;
    return {};
}

bool CheckClassResolution()
{
    const FBlock Defaults{ .Fields = { { "Altitude", "4" }, { "Library-only", "1" } } };
    FClassLibrary Classes;
    Classes.emplace("parent", FClassDefinition{ .Declared = {
        .Fields = { { "Altitude", "8" } },
        .Children = {
            { .Kind = "Energy", .Fields = { { "Initial", "10" }, { "Max", "20" } } },
            { .Kind = "TYPE", .Name = "Explosion", .Fields = { { "Name", "First" } } },
            { .Kind = "TYPE", .Name = "Explosion", .Fields = { { "Name", "Second" } } },
        },
    } });
    Classes.emplace("child", FClassDefinition{ .Defaults = Defaults, .Declared = {
        .Fields = { { "Derived-From", "parent" }, { "Altitude", "12" } },
        .Children = {
            { .Kind = "Energy", .Fields = { { "Initial", "15" } } },
            { .Kind = "TYPE", .Name = "Explosion", .Fields = { { "Name", "Third" } } },
        },
    } });
    if (!ResolveClass(Classes, "child"))
    {
        return false;
    }

    FBlock Result = Classes.at("child").Resolved;
    if (Result.Find("Altitude") != "12" || !Result.Find("Library-only").empty() || Result.Children.size() != 4 ||
        Result.Children[0].Find("Initial") != "15" || Result.Children[0].Find("Max") != "20")
    {
        return false;
    }

    MergeProperties(Result, { .Fields = { { "Altitude", "16" } }, .Children = {
        { .Kind = "TYPE", .Name = "Explosion", .Fields = { { "Name", "Instance" } } },
    } }, true);
    if (Result.Find("Altitude") != "16" || Result.Children.size() != 4 || Result.Children.back().Find("Name") != "Instance")
    {
        return false;
    }

    MergeProperties(Result, { .Fields = { { "Override-Attributes", "" } } });
    if (Result.Children.size() != 1)
    {
        return false;
    }

    MergeProperties(Result, { .Fields = { { "HAVE\tGun", "" }, { "RANDOM Ammo 1 2", "" } } });
    MergeProperties(Result, { .Fields = { { "HAVE\tGun", "" } } });
    if (std::ranges::count_if(Result.Fields, [](const auto& Field) { return Field.first == "HAVE\tGun"; }) != 2)
    {
        return false;
    }

    MergeProperties(Result, { .Fields = { { "Override-Objects", "" }, { "Override-Random-Objects", "" } } });
    if (std::ranges::any_of(Result.Fields, [](const auto& Field) { return Field.first == "HAVE\tGun" || Field.first == "RANDOM Ammo 1 2"; }))
    {
        return false;
    }

    Classes.emplace("cycle", FClassDefinition{ .Declared = { .Fields = { { "Derived-From", "cycle" } } } });
    Classes.emplace("missing", FClassDefinition{ .Declared = { .Fields = { { "Derived-From", "unknown" } } } });
    return !ResolveClass(Classes, "cycle") && !ResolveClass(Classes, "missing");
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
        Level.TileReferences.emplace_back(*Flag, std::string(Source));
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
    const std::optional<F3diHeader> Header = File ? ReadStruct<F3diHeader>(*File) : std::nullopt;
    if (!Header)
    {
        return std::unexpected(std::format("{} missing or truncated", Path));
    }
    if ((File->size() - sizeof(F3diHeader)) % sizeof(F3diRecord) != 0)
    {
        return std::unexpected(std::format("{}: trailing bytes", Path));
    }
    Level.WorldExtentX = Header->WorldExtentX;
    Level.WorldExtentY = Header->WorldExtentY;
    const uint32_t Width = GridWidthFromExtent(Level.WorldExtentX);
    const uint32_t Height = GridHeightFromExtent(Level.WorldExtentY);
    Level.Cells.assign(size_t{ Width } * Height, {});

    // The original does not bounds-check cell coordinates; out-of-range records are rejected here.
    for (size_t Offset = sizeof(F3diHeader); Offset < File->size(); Offset += sizeof(F3diRecord))
    {
        const F3diRecord Record = *ReadStruct<F3diRecord>(*File, Offset);
        if (Record.X >= Width || Record.Y >= Height)
        {
            return std::unexpected(std::format("{}: cell ({}, {}) outside {}x{} grid", Path, Record.X, Record.Y, Width, Height));
        }
        if (Record.TileId < 0 || size_t(Record.TileId) >= Level.TileTypes.size())
        {
            ++Level.MissingTileRefs;
            continue;
        }
        if (const std::optional<size_t> Slot = TerrainSlot(Level.TileTypes[Record.TileId]))
        {
            Level.Cells[size_t{ Record.X } * Height + Record.Y][*Slot] = uint16_t(Record.TileId);
        }
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
        if (ReadStruct<uint32_t>(Slot) == Gaf::Tag("BMP_"))
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

    const std::vector<uint8_t>& Metadata = (*BlockSlots)[BlockMetadataSlot];
    if (Metadata.size() != Level.Blocks.size() * sizeof(FBlockMetadata))
    {
        return std::unexpected(std::format("{}: BLK_ metadata count mismatch", Path));
    }

    Level.BlockDepths.reserve(Level.Blocks.size());
    for (size_t Index = 0; Index < Level.Blocks.size(); ++Index)
    {
        const auto Block = ReadStruct<FBlockMetadata>(Metadata, Index * sizeof(FBlockMetadata));
        if (!Block || Block->Depth < BackgroundTerrainDepth || Block->Depth > MaxTerrainDepth)
        {
            return std::unexpected(std::format("{}: invalid BLK_ depth at {}", Path, Index));
        }

        Level.BlockDepths.push_back(Block->Depth);
    }

    const std::vector<uint8_t>& HeaderSlot = (*MapSlots)[MapHeaderSlot];
    const std::vector<uint8_t>& Cells = (*MapSlots)[MapCellSlot];
    const std::optional<FMapHeader> Header = ReadStruct<FMapHeader>(HeaderSlot);
    if (!Header || HeaderSlot.size() != sizeof(FMapHeader))
    {
        return std::unexpected(std::format("{}: MAP_ header is {} bytes", Path, HeaderSlot.size()));
    }
    Level.LayerCount = Header->LayerCount;
    Level.GridWidth = Header->GridWidth;
    Level.GridHeight = Header->GridHeight;
    if (Level.LayerCount == 0 || Level.LayerCount > MaxLayers || Level.GridWidth == 0 || Level.GridWidth > MaxGridDimension ||
        Level.GridHeight == 0 || Level.GridHeight > MaxGridDimension ||
        Cells.size() != size_t{ Level.LayerCount } * Level.GridWidth * Level.GridHeight * sizeof(uint32_t))
    {
        return std::unexpected(std::format("{}: MAP_ size mismatch", Path));
    }
    Level.Map.resize(Cells.size() / sizeof(uint32_t));
    std::memcpy(Level.Map.data(), Cells.data(), Cells.size());
    if (std::ranges::any_of(Level.Map, [&](const uint32_t Block) { return Block > Level.Blocks.size(); }))
    {
        return std::unexpected(std::format("{}: MAP_ references a missing block", Path));
    }
    return {};
}

std::optional<float> ParseFloat(const std::string_view Text)
{
    const std::string_view Number = Trim(Text);
    if (Number.empty())
    {
        return std::nullopt;
    }

    float Value = 0.f;
    const auto [End, Error] = std::from_chars(Number.data(), Number.data() + Number.size(), Value);
    if (Error != std::errc{} || End != Number.data() + Number.size() || !std::isfinite(Value))
    {
        return std::nullopt;
    }

    return Value;
}

std::optional<std::pair<float, float>> ParsePosition(const std::string_view Text)
{
    const size_t Comma = Text.find(',');
    if (Comma == std::string_view::npos)
    {
        return std::nullopt;
    }
    const std::optional<float> X = ParseFloat(Text.substr(0, Comma));
    const std::optional<float> Y = ParseFloat(Text.substr(Comma + 1));
    if (!X || !Y)
    {
        return std::nullopt;
    }

    // Object initialization at 0041a870 truncates positions to signed 32-bit coordinates.
    constexpr double MinCoordinate = std::numeric_limits<int32_t>::min();
    constexpr double MaxCoordinate = std::numeric_limits<int32_t>::max();
    if (double(*X) < MinCoordinate || double(*X) > MaxCoordinate || double(*Y) < MinCoordinate || double(*Y) > MaxCoordinate)
    {
        return std::nullopt;
    }
    return std::pair{ *X, *Y };
}

// ParseObjectDirection at 00407970: eight compass points with three intermediate steps each.
std::optional<int> ParseDirection(const std::string_view Text)
{
    if (Text.empty())
    {
        return ObjectDirectionCount;
    }

    static constexpr std::string_view Directions[] = { "sw", "s", "se", "e", "ne", "n", "nw", "w" };
    constexpr int CompassStep = ObjectDirectionCount / std::size(Directions);
    const std::string Lower = ToLower(Text);
    const size_t Separator = Lower.find('_');
    const std::string_view Compass = std::string_view(Lower).substr(0, Separator);
    const auto Found = std::ranges::find(Directions, Compass);
    const std::optional<int> Offset = Separator == std::string::npos ? std::optional<int>(0) : ParseInt(std::string_view(Lower).substr(Separator + 1));
    if (Found == std::end(Directions) || !Offset || *Offset < 0 || *Offset >= CompassStep)
    {
        return std::nullopt;
    }

    return int(Found - std::begin(Directions)) * CompassStep + *Offset;
}

std::optional<int> ParseAnimationIndex(const std::string_view Text)
{
    const std::optional<int> Index = Text.empty() ? std::optional<int>(0) : ParseInt(Text);
    return Index && *Index >= 0 && *Index <= MaxObjectAnimationIndex ? Index : std::nullopt;
}

bool CheckLevelProperties()
{
    return CheckClassResolution() && ParsePosition("2147483520, -2147483648") && !ParsePosition("2147483648, 0") &&
        !ParsePosition("0, -2147483904") && !ParsePosition("nan, 0") && ParseDirection("SE_2") == 8 && !ParseDirection("SE_3") &&
        ParseAnimationIndex("1") == 1 && !ParseAnimationIndex("-1") && !ParseAnimationIndex("2147483647");
}

// LoadObjectGaf at 00403fe0 searches the current zone before COMMON, independently of the class library's origin.
std::expected<std::string, std::string> ResolveGafPath(const std::vector<std::string>& Names, const std::string_view Zone, const std::string_view Name)
{
    if (Name.empty())
    {
        return std::string();
    }

    for (const std::string_view Root : { Zone, std::string_view("common") })
    {
        const std::string Path = ToLower(std::format("{}\\data\\{}", Root, Name));
        if (std::ranges::find(Names, Path) != Names.end())
        {
            return Path;
        }
    }

    return std::unexpected(std::format("missing object GAF '{}' in {} or common", Name, Zone));
}
}

std::optional<float> ParseLevelFloat(const std::string_view Text)
{
    return ParseFloat(Text);
}

std::expected<FLevelPropertyBlock, std::string> ParseLevelBlocks(const std::string_view Text)
{
    return ParseBlocks(Text);
}

std::string_view FLevelPropertyBlock::Find(const std::string_view Field) const
{
    const std::string LowerField = ToLower(Field);
    for (const auto& [Key, Value] : std::views::reverse(Fields))
    {
        if (ToLower(Key) == LowerField)
        {
            return Value;
        }
    }

    return {};
}

namespace
{
// The rendering fields of an object of the class whose resolved properties are given; the position stays at zero.
std::expected<FLevelObject, std::string> BuildObject(const std::vector<std::string>& ArchiveNames, const std::string_view Zone, const std::string& ClassName,
    FBlock Properties)
{
    const std::string_view AltitudeText = Properties.Find("Altitude");
    const std::optional<float> Altitude = AltitudeText.empty() ? std::optional<float>(0.f) : ParseFloat(AltitudeText);
    const auto IntegerProperty = [&](const std::string_view Field)
    {
        const std::string_view Value = Properties.Find(Field);
        return Value.empty() ? std::optional<int>(0) : ParseInt(Value);
    };
    const std::optional<int> GafIndex = ParseAnimationIndex(Properties.Find("GAF-index"));
    const std::optional<int> ShadowIndex = ParseAnimationIndex(Properties.Find("Shadow-index"));
    const std::optional<int> BoundingBoxSize = IntegerProperty("Bounding-box-size");
    const std::optional<int> InGround = IntegerProperty("In-ground");
    const std::optional<int> ZShadow = IntegerProperty("Z-shadow");
    const std::optional<int> Direction = ParseDirection(Properties.Find("Dir"));
    if (!Altitude || !GafIndex || !ShadowIndex || !BoundingBoxSize || *BoundingBoxSize < 0 || !InGround || !ZShadow || !Direction)
    {
        return std::unexpected(std::format("invalid rendering properties for '{}'", ClassName));
    }

    const std::expected<std::string, std::string> GafPath = ResolveGafPath(ArchiveNames, Zone, Properties.Find("GAF-name"));
    const std::expected<std::string, std::string> ShadowGafPath = ResolveGafPath(ArchiveNames, Zone, Properties.Find("Shadow-GAF"));
    if (!GafPath || !ShadowGafPath)
    {
        return std::unexpected(std::format("class '{}': {}", ClassName, !GafPath ? GafPath.error() : ShadowGafPath.error()));
    }

    const std::string_view SubTypeText = Properties.Find("Sub-type");
    const std::string SubType = ToLower(SubTypeText.empty() ? ParentDirectory(Properties.Find("GAF-name")) : SubTypeText);
    // AnimInitializeGraphics at 00417b30 allocates 24 consecutive directional animations for these subtypes.
    static constexpr std::string_view DirectionalTypes[] = { "ammo", "cop", "vyruz", "walker", "burner", "jeep", "roller", "turret", "tank" };
    return FLevelObject{
        .ClassName = ClassName,
        .Altitude = *Altitude,
        .BoundingBoxSize = *BoundingBoxSize,
        .Behaviour = std::string(Properties.Find("Behaviour")),
        .GafPath = *GafPath,
        .ShadowGafPath = *ShadowGafPath,
        .GafIndex = *GafIndex,
        .ShadowIndex = *ShadowIndex,
        .Direction = *Direction,
        .bInGround = *InGround != 0,
        .bZShadow = *ZShadow != 0,
        .bDirectional = std::ranges::find(DirectionalTypes, SubType) != std::end(DirectionalTypes),
        .bDetail = IntegerProperty("Is-detail").value_or(0) != 0,
        // Only-base (object +0x128 bit 0x20): a session's wreck of a two-piece object.
        .bWrecked = IntegerProperty("Only-base").value_or(0) != 0,
        .SubType = SubType,
        .Properties = std::move(Properties),
    };
}
}

std::optional<size_t> CreateLevelObject(FLevel& Level, const std::string_view ClassName, const float X, const float Y)
{
    const auto Template = Level.ClassObjects.find(ToLower(ClassName));
    if (Template == Level.ClassObjects.end())
    {
        return std::nullopt;
    }

    FLevelObject Object = Template->second;
    Object.X = X;
    Object.Y = Y;
    Level.Objects.push_back(std::move(Object));
    return Level.Objects.size() - 1;
}

std::expected<FLevel, std::string> LoadLevel(FResourceArchive& Archive, const std::string_view LevelPath, const std::optional<std::string_view> SessionObjects)
{
    static const bool bClassesChecked = CheckLevelProperties();
    assert(bClassesChecked);
    if (!bClassesChecked)
    {
        return std::unexpected("class resolution self-check failed");
    }

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

    const std::expected<std::string, std::string> ObjectText = SessionObjects ? std::expected<std::string, std::string>(std::string(*SessionObjects)) : ReadText(Archive, Base + ".3do");
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
    if (SessionObjects)
    {
        Level.DifficultyLevel = ParseInt(Objects->Find("DIFFICULTY-LEVEL"));
    }

    FClassLibrary Classes;
    const std::vector<std::string> ArchiveNames = Archive.GetNames();
    const std::string CommonPrefix = "common\\";
    const std::string ZonePrefix = ToLower(Zone) + "\\";
    for (const std::string& Name : ArchiveNames)
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
        FBlock Defaults;
        for (const FBlock& Block : Library->Children)
        {
            if (ToLower(Block.Kind) == "default-property")
            {
                MergeProperties(Defaults, Block);
                continue;
            }

            if (ToLower(Block.Kind) != "object-class")
            {
                continue;
            }
            const std::string ClassName = ToLower(Block.Find("Name"));
            if (ClassName.empty())
            {
                return std::unexpected(std::format("{}: unnamed class", Name));
            }

            if (!Classes.emplace(ClassName, FClassDefinition{ .Defaults = Defaults, .Declared = Block }).second)
            {
                return std::unexpected(std::format("{}: duplicate class '{}'", Name, ClassName));
            }
        }
    }
    for (const auto& [ClassName, Class] : Classes)
    {
        if (std::expected<void, std::string> Result = ResolveClass(Classes, ClassName); !Result)
        {
            return std::unexpected(Result.error());
        }
    }

    for (const FBlock& Block : Objects->Children)
    {
        if (ToLower(Block.Kind) != "create-object")
        {
            continue;
        }

        const std::string ClassName(Block.Find("Class-name"));
        const auto Class = Classes.find(ToLower(ClassName));
        if (Class == Classes.end())
        {
            return std::unexpected(std::format("{}.3do: unknown class '{}'", Base, ClassName));
        }

        // In a .3do an instance's HAVE names an object it carries, not contents as a class's does.
        FBlock Instance = Block;
        std::vector<std::string> CarriedNames;
        std::erase_if(Instance.Fields, [&](const auto& Field)
        {
            const bool bHave = Field.second.empty() && ToLower(Field.first.substr(0, Field.first.find_first_of(" \t"))) == "have";
            if (bHave)
            {
                CarriedNames.emplace_back(Trim(std::string_view(Field.first).substr(Field.first.find_first_of(" \t") + 1)));
            }

            return bHave;
        });
        FBlock Properties = Class->second.Resolved;
        MergeProperties(Properties, Instance, true);
        const std::optional<std::pair<float, float>> Position = ParsePosition(Properties.Find("Position"));
        std::expected<FLevelObject, std::string> Object = BuildObject(ArchiveNames, Zone, ClassName, std::move(Properties));
        if (!Position || !Object)
        {
            return std::unexpected(std::format("{}.3do: {}", Base, Object ? std::format("invalid position for '{}'", ClassName) : Object.error()));
        }

        Object->X = Position->first;
        Object->Y = Position->second;
        Object->FileIndex = Level.Objects.size();
        Object->CarriedNames = std::move(CarriedNames);
        Level.Objects.push_back(std::move(*Object));
    }

    for (auto& [ClassName, Class] : Classes)
    {
        if (const std::expected<std::string, std::string> GafPath = ResolveGafPath(ArchiveNames, Zone, Class.Resolved.Find("GAF-name")); GafPath && !GafPath->empty())
        {
            Level.ClassGafPaths.emplace(ClassName, *GafPath);
        }

        // Classes that cannot be drawn, such as abstract bases, get no template.
        if (std::expected<FLevelObject, std::string> Template = BuildObject(ArchiveNames, Zone, std::string(Class.Resolved.Find("Name")), Class.Resolved); Template)
        {
            Level.ClassObjects.emplace(ClassName, std::move(*Template));
        }

        Level.Classes.emplace(ClassName, std::move(Class.Resolved));
    }

    if (!SessionObjects && ParseInt(Objects->Find("OBJECT-COUNT")) != int(Level.Objects.size()))
    {
        return std::unexpected(std::format("{}.3do: OBJECT-COUNT does not match {} objects", Base, Level.Objects.size()));
    }

    const std::expected<FBlock, std::string> Paths = ReadText(Archive, Base + ".3dp").and_then(ParseBlocks);
    if (!Paths)
    {
        return std::unexpected(std::format("{}.3dp: {}", Base, Paths.error()));
    }

    for (const FBlock& Block : Paths->Children)
    {
        if (Block.Kind != "CREATE-PATH")
        {
            continue;
        }

        FPatrolPath& Path = Level.PatrolPaths.emplace_back(FPatrolPath{ .Id = ParseInt(Block.Find("Id")).value_or(0) });
        for (const auto& [Field, Value] : Block.Fields)
        {
            if (Field != "Point")
            {
                continue;
            }

            const size_t Comma = Value.find(',');
            const std::optional<int> X = ParseInt(std::string_view(Value).substr(0, Comma));
            const std::optional<int> Y = Comma == std::string::npos ? std::nullopt : ParseInt(std::string_view(Value).substr(Comma + 1));

            if (!X || !Y)
            {
                return std::unexpected(std::format("{}.3dp: invalid point '{}' in path {}", Base, Value, Path.Id));
            }

            Path.Points.push_back({ *X, *Y });
        }

        if (ParseInt(Block.Find("Count")) != int(Path.Points.size()))
        {
            return std::unexpected(std::format("{}.3dp: path {} Count does not match its points", Base, Path.Id));
        }
    }

    // LevelGet3dgPath at 0041ebe0 prefers the .3DO 3DG-FILENAME, relative to the zone folder.
    const std::string_view GraphicsName = Objects->Find("3DG-FILENAME");
    const std::string GraphicsPath = GraphicsName.empty() ? Base + ".3dg" : std::format("{}\\{}", Zone, GraphicsName);
    Level.GraphicsName = GraphicsName.empty() ? Base.substr(Separator + 1) + ".3dg" : std::string(GraphicsName);
    if (std::expected<void, std::string> Result = Load3dg(Archive, GraphicsPath, Level); !Result)
    {
        return std::unexpected(Result.error());
    }
    if (Level.GridWidth != GridWidthFromExtent(Level.WorldExtentX) || Level.GridHeight != GridHeightFromExtent(Level.WorldExtentY))
    {
        return std::unexpected(std::format("{}: MAP_ grid {}x{} does not match .3DI extents", GraphicsPath, Level.GridWidth, Level.GridHeight));
    }
    return Level;
}
