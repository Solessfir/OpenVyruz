#pragma once

#include "Pg/Gaf.h"

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

class FResourceArchive;

// Size of one .3DG block and one .3DI/MAP_ grid cell in world pixels.
inline constexpr uint32_t BlockWidth = 64;
inline constexpr uint32_t BlockHeight = 32;
inline constexpr int32_t BackgroundTerrainDepth = -1;
inline constexpr int32_t MaxTerrainDepth = 4;

// .3DG MAP_ slot 0.
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

// .3DG MAP_ slot 2, one per layer: the size of the layer's cells in bytes, which GafParseSlot3 (0047bdb0) copies them by, and its grid size; the rest is kept as written.
struct FMapLayerRecord
{
    uint32_t CellBytes;
    uint32_t Width;
    uint32_t Height;
    uint32_t Reserved[16];
};
static_assert(sizeof(FMapLayerRecord) == 76);

// .3DG BLK_ slot 0, one per block; GafParseSlot0 at 0047b830 copies the final word to the runtime block's depth.
struct FBlockMetadata
{
    uint32_t Flags;
    int32_t OffsetX;
    int32_t OffsetY;
    int32_t Depth;
};
static_assert(sizeof(FBlockMetadata) == 16);

// .3DI: the world extents, then one record per filled cell slot.
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

inline constexpr size_t MapHeaderSlot = 0;
inline constexpr size_t MapLayerSlot = 2;
inline constexpr size_t MapCellSlot = 3;
inline constexpr size_t BlockMetadataSlot = 0;
inline constexpr size_t BlockBitmapSlot = 1;

// Grid size covering a world extent, as computed in LevelLoadFiles at 00429710.
constexpr uint32_t GridWidthFromExtent(const uint32_t ExtentX)
{
    return ExtentX / BlockWidth + 1;
}

constexpr uint32_t GridHeightFromExtent(const uint32_t ExtentY)
{
    return ExtentY / BlockHeight + 1;
}

// Semantic tile types; values are the type IDs registered by VzCreateTypeTable at 00447430.
enum class ETerrain : uint8_t
{
    None,
    Bridge,
    Corner,
    Ground,
    GroundLeftWall,
    GroundRightWall,
    HalfGround,
    HalfRoof,
    HorizontalWall,
    LeftWall,
    Obstacle,
    RightWall,
    Roof,
    RoofPlus,
    WallCorner,
    Water,
};

// Ordered properties retain repeated fields, named TYPE/ADD blocks, and bare directives.
struct FLevelPropertyBlock
{
    std::string_view Find(std::string_view Field) const;

    std::string Kind;
    std::string Name;
    std::vector<std::pair<std::string, std::string>> Fields;
    std::vector<FLevelPropertyBlock> Children;
};

inline constexpr int ObjectDirectionCount = 24;

struct FLevelObject
{
    std::string ClassName;
    float X = 0.f;
    float Y = 0.f;
    float Altitude = 0.f;
    int BoundingBoxSize = 0;
    std::string Behaviour;
    std::string GafPath;
    std::string ShadowGafPath;
    // Original 1-based ANM_ indices; zero disables that animation.
    int GafIndex = 0;
    int ShadowIndex = 0;
    // ObjectDirectionCount means no .3DO direction override.
    int Direction = ObjectDirectionCount;
    bool bInGround = false;
    bool bZShadow = false;
    bool bDirectional = false;
    // Is-detail (class flag 0x100000): ObjectDraw leaves the object out with low details.
    bool bDetail = false;
    // Collected pickups and destroyed objects stay in the list so indices remain stable.
    bool bRemoved = false;
    // Object +0x128 bit 0x20: a Two-pieces object's first death left its lower half, which no longer acts and can be destroyed again.
    bool bWrecked = false;
    // A tank's 1-based top pose, the frame of its top animation; zero until its behaviour turns it.
    int TopPose = 0;
    // The object's place in the level's .3DO; SIZE_MAX for one created during play.
    size_t FileIndex = SIZE_MAX;
    std::string SubType;
    FLevelPropertyBlock Properties;
    // A session's HAVE lines in the object's block ("HAVE <name>" or "HAVE <name> <timeout>"), which name objects written before it that it carries.
    std::vector<std::string> CarriedNames;
};

// A .3DP CREATE-PATH: patrol waypoints in world pixels, kept as the integers the path table stores.
struct FPatrolPath
{
    int Id = 0;
    std::vector<std::array<int32_t, 2>> Points;
};

struct FLevel
{
    // Points into Cells, column-major as allocated by LevelAllocGrid at 00429390.
    const std::array<uint16_t, 4>& GetCell(const uint32_t X, const uint32_t Y) const { return Cells[size_t{ X } * GridHeight + Y]; }

    std::string Name;
    std::string Id;

    // .3DG: 64x32 BLK_ bitmaps and the layered MAP_ grid of 1-based block indices (0 = empty), layer-major then row-major.
    std::vector<Gaf::FFrame> Blocks;
    std::vector<int32_t> BlockDepths;
    uint32_t LayerCount = 0;
    uint32_t GridWidth = 0;
    uint32_t GridHeight = 0;
    std::vector<uint32_t> Map;

    // .3DX: animated map GAFs (archive paths) and the semantic type of each 1-based tile descriptor (index 0 unused).
    std::vector<std::string> AnimGafs;
    // The same GAFs as the .3DX writes them, editor paths of which only the last three components count.
    std::vector<std::string> AnimGafSources;
    std::vector<ETerrain> TileTypes;
    // Each descriptor's line in .3DX order: a 1-based AnimGafs index (0 for a bitmap path) and the name after it, an animation of that GAF.
    std::vector<std::pair<int, std::string>> TileReferences;

    // .3DI: four semantic slots per cell holding tile descriptor indices (see Docs/DataFormats.md).
    uint32_t WorldExtentX = 0;
    uint32_t WorldExtentY = 0;
    std::vector<std::array<uint16_t, 4>> Cells;
    uint32_t MissingTileRefs = 0;

    // .3DO
    std::vector<FLevelObject> Objects;
    // Its 3DG-FILENAME, else <name>.3dg, as a session writes it; and a session's DIFFICULTY-LEVEL.
    std::string GraphicsName;
    std::optional<int> DifficultyLevel;

    // .3DP, in file order.
    std::vector<FPatrolPath> PatrolPaths;

    // Resolved COMMON and zone classes by lowercase name, for objects created at run time such as ammo.
    std::unordered_map<std::string, FLevelPropertyBlock> Classes;
    // Archive path of each class's GAF-name, for the classes whose GAF exists.
    std::unordered_map<std::string, std::string> ClassGafPaths;
    // A fresh object of each drawable class, for objects created at run time such as dropped items.
    std::unordered_map<std::string, FLevelObject> ClassObjects;
};

// Adds an object of the class (any case) at the position, as 3do::CreateObject (00413800) does at run time; returns its index.
std::optional<size_t> CreateLevelObject(FLevel& Level, std::string_view ClassName, float X, float Y);

// Loads <Zone>\<Name>.3DX/.3DI/.3DO/.3DP/.3DG plus the COMMON and zone .3DC class libraries, e.g. "UNDERGROUND\T1U1".
// Mirrors VzLoadLevel (0041f7c0) -> LevelLoadFiles (00429710) and LevelLoad3dg (0041eae0).
// A session (SessionLoad) gives its own .3do text in place of the level's, whose OBJECT-COUNT leaves out the carried objects.
std::expected<FLevel, std::string> LoadLevel(FResourceArchive& Archive, std::string_view LevelPath, std::optional<std::string_view> SessionObjects = std::nullopt);

// Parses one numeric property value the way the class loader stores it (a finite float).
std::optional<float> ParseLevelFloat(std::string_view Text);
// The block grammar of .3DC, .3DO and .3DP text: a root of top-level fields and blocks, comments dropped.
std::expected<FLevelPropertyBlock, std::string> ParseLevelBlocks(std::string_view Text);
