#pragma once

#include "Pg/Gaf.h"

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class FResourceArchive;

// Size of one .3DG block and one .3DI/MAP_ grid cell in world pixels.
inline constexpr uint32_t BlockWidth = 64;
inline constexpr uint32_t BlockHeight = 32;

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
    std::string SubType;
    FLevelPropertyBlock Properties;
};

struct FLevel
{
    // Points into Cells, column-major as allocated by LevelAllocGrid at 00429390.
    const std::array<uint16_t, 4>& GetCell(const uint32_t X, const uint32_t Y) const { return Cells[size_t{ X } * GridHeight + Y]; }

    std::string Name;
    std::string Id;

    // .3DG: 64x32 BLK_ bitmaps and the layered MAP_ grid of 1-based block indices (0 = empty), layer-major then row-major.
    std::vector<Gaf::FFrame> Blocks;
    uint32_t LayerCount = 0;
    uint32_t GridWidth = 0;
    uint32_t GridHeight = 0;
    std::vector<uint32_t> Map;

    // .3DX: animated map GAFs (archive paths) and the semantic type of each 1-based tile descriptor (index 0 unused).
    std::vector<std::string> AnimGafs;
    std::vector<ETerrain> TileTypes;

    // .3DI: four semantic slots per cell holding tile descriptor indices (see Docs/DataFormats.md).
    uint32_t WorldExtentX = 0;
    uint32_t WorldExtentY = 0;
    std::vector<std::array<uint16_t, 4>> Cells;
    uint32_t MissingTileRefs = 0;

    // .3DO
    std::vector<FLevelObject> Objects;
};

// Loads <Zone>\<Name>.3DX/.3DI/.3DO/.3DG plus the COMMON and zone .3DC class libraries, e.g. "UNDERGROUND\T1U1".
// Mirrors VzLoadLevel (0041f7c0) -> LevelLoadFiles (00429710) and LevelLoad3dg (0041eae0).
std::expected<FLevel, std::string> LoadLevel(FResourceArchive& Archive, std::string_view LevelPath);
