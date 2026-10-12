#include "Game/Collision.h"
#include "Game/Engine.h"
#include "Game/Level.h"
#include "Game/Loop.h"
#include "Pg/File.h"
#include "Pg/Gaf.h"
#include "Pg/ResourceArchive.h"
#include "Render/Renderer.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
constexpr int64_t SpriteDepthLayerCount = 4;
constexpr int64_t SpriteDepthLayerMask = SpriteDepthLayerCount - 1;
constexpr int64_t BodyDepthLayer = 0;
constexpr int64_t ShadowDepthLayer = 1;
constexpr int64_t MiddleDepthLayer = 2;
constexpr int64_t InGroundDepthLayer = 3;
constexpr int64_t ZShadowDepthOffset = 64;
constexpr uint32_t AnimationBlendMask = 0xFE;
constexpr uint32_t AdditiveDrawMode = 6;
constexpr uint32_t InverseSourceDrawMode = 8;
constexpr int FourWayDirectionCount = 4;
constexpr std::string_view TankKind = "tank";

// ObjectDraw at 00431ae0 draws a walker's body (kind 0x19) 32 pixels above its other sprites.
float BodyLift(const FLevelObject& Object, const bool bShadow)
{
    constexpr float WalkerLift = 32.f;
    return !bShadow && Object.SubType == "walker" ? WalkerLift : 0.f;
}
constexpr uint32_t FirstOccludingMapLayer = 2;

// ObjectDraw at 00431d60 truncates world + 0.5 before applying scroll and altitude.
constexpr int64_t SpriteCoordinate(const float World)
{
    return int64_t(double(World) + 0.5);
}
static_assert(SpriteCoordinate(2.49f) == 2);
static_assert(SpriteCoordinate(2.5f) == 3);
static_assert(SpriteCoordinate(-2.5f) == -2);

// ObjectDraw at 00431ae0 gives In-ground precedence over the Z-shadow depth bias.
constexpr int64_t SpriteDepth(const int64_t GroundY, const bool bShadow, const bool bInGround, const bool bZShadow)
{
    const bool bShiftDepth = !bShadow && !bInGround && bZShadow;
    const int64_t DepthY = bShiftDepth ? std::max(int64_t{ 0 }, GroundY - ZShadowDepthOffset) : GroundY;
    const int64_t Layer = bShadow || bShiftDepth ? ShadowDepthLayer : bInGround ? InGroundDepthLayer : BodyDepthLayer;
    return DepthY * SpriteDepthLayerCount + Layer;
}
static_assert(SpriteDepth(100, false, false, false) == 400);
static_assert(SpriteDepth(100, true, true, true) == 401);
static_assert(SpriteDepth(100, false, true, true) == 403);
static_assert(SpriteDepth(100, false, false, true) == 145);
static_assert(SpriteDepth(32, false, false, true) == 1);

// LevelCompositeSprites at 00420070 flushes all layer-3 ground decals before shadows and bodies.
constexpr bool SpriteDrawsBefore(const int64_t LeftDepth, const int64_t RightDepth)
{
    const bool bLeftGround = (LeftDepth & SpriteDepthLayerMask) == InGroundDepthLayer;
    const bool bRightGround = (RightDepth & SpriteDepthLayerMask) == InGroundDepthLayer;
    return bLeftGround != bRightGround ? bLeftGround : LeftDepth < RightDepth;
}
static_assert(SpriteDrawsBefore(SpriteDepth(2915, false, true, true), SpriteDepth(2903, true, false, false)));
static_assert(!SpriteDrawsBefore(SpriteDepth(2903, true, false, false), SpriteDepth(2915, false, true, true)));
static_assert(SpriteDrawsBefore(SpriteDepth(2903, false, false, false), SpriteDepth(2903, true, false, false)));

int ObjectPoseCount(const FLevelObject& Object, const bool bShadow)
{
    if (Object.SubType == "spider" || (!bShadow && Object.SubType == "heli"))
    {
        return FourWayDirectionCount;
    }

    return !bShadow && Object.bDirectional ? ObjectDirectionCount : 1;
}

size_t ObjectAnimationIndex(const FLevelObject& Object, const bool bShadow)
{
    const int PoseCount = ObjectPoseCount(Object, bShadow);
    const int Direction = Object.Direction < ObjectDirectionCount ? Object.Direction : 0;
    // HeliSelectBodyPose at 004334e0 picks a heli's animation by speed rather than facing; the facing selects the frame instead.
    const int Offset = PoseCount > 1 && Object.SubType != "heli" ? Direction / (ObjectDirectionCount / PoseCount) : !bShadow && Object.SubType == "heli" ? Object.Tilt : 0;
    return size_t((bShadow ? Object.ShadowIndex : Object.GafIndex) - 1 + Offset);
}

bool IsSupportedDrawMode(const uint32_t Flags)
{
    const uint32_t Mode = Flags & AnimationBlendMask;
    return Mode == 0 || Mode == AdditiveDrawMode || Mode == InverseSourceDrawMode;
}

struct FObjectGaf
{
    std::vector<Gaf::FFrame> Bitmaps;
    std::vector<Gaf::FAnimation> Animations;
};

std::optional<FObjectGaf> LoadObjectGaf(FResourceArchive& Archive, const std::string_view Path)
{
    const std::optional<std::vector<uint8_t>> File = Archive.Read(Path);
    if (!File)
    {
        return std::nullopt;
    }

    std::optional<std::vector<Gaf::FFrame>> Bitmaps = Gaf::LoadFrames(*File);
    if (!Bitmaps)
    {
        return std::nullopt;
    }

    std::optional<std::vector<Gaf::FAnimation>> Animations = Gaf::LoadAnimations(*File, Bitmaps->size());
    if (!Animations)
    {
        return std::nullopt;
    }

    return FObjectGaf{ .Bitmaps = std::move(*Bitmaps), .Animations = std::move(*Animations) };
}

// Decodes every .gaf in the archive; any failure means the loader or format notes are wrong.
int VerifyGafs(FResourceArchive& Archive)
{
    int Decoded = 0;
    int Failed = 0;
    size_t FrameCount = 0;
    for (const std::string& Name : Archive.GetNames())
    {
        if (!Name.ends_with(".gaf"))
        {
            continue;
        }
        const std::optional<FObjectGaf> Gaf = LoadObjectGaf(Archive, Name);
        if (!Gaf)
        {
            std::printf("FAIL %s\n", Name.c_str());
            ++Failed;
            continue;
        }
        ++Decoded;
        FrameCount += Gaf->Bitmaps.size();
    }
    std::printf("GAF: %d decoded (%zu frames), %d failed\n", Decoded, FrameCount, Failed);
    return Failed == 0 ? 0 : 1;
}

// Loads every level in the archive plus the animated map GAFs its .3DX references.
int VerifyLevels(FResourceArchive& Archive)
{
    int Loaded = 0;
    int Failed = 0;
    std::unordered_map<std::string, std::vector<Gaf::FAnimation>> CheckedGafs;
    for (const std::string& Name : Archive.GetNames())
    {
        if (!Name.ends_with(".3do"))
        {
            continue;
        }
        const std::string LevelPath = Name.substr(0, Name.size() - 4);
        const std::expected<FLevel, std::string> Level = LoadLevel(Archive, LevelPath);
        if (!Level)
        {
            std::printf("FAIL %s: %s\n", LevelPath.c_str(), Level.error().c_str());
            ++Failed;
            continue;
        }
        bool bGafsOk = true;
        for (const FLevelObject& Object : Level->Objects)
        {
            for (const std::string* const Path : { &Object.GafPath, &Object.ShadowGafPath })
            {
                if (!Path->empty() && !CheckedGafs.contains(*Path))
                {
                    std::optional<FObjectGaf> Gaf = LoadObjectGaf(Archive, *Path);
                    if (!Gaf)
                    {
                        std::printf("FAIL %s: object GAF %s\n", LevelPath.c_str(), Path->c_str());
                        bGafsOk = false;
                    }
                    else
                    {
                        CheckedGafs.emplace(*Path, std::move(Gaf->Animations));
                    }
                }

                const int BaseIndex = Path == &Object.GafPath ? Object.GafIndex : Object.ShadowIndex;
                const bool bShadow = Path == &Object.ShadowGafPath;
                const int PoseCount = ObjectPoseCount(Object, bShadow);
                if (BaseIndex > 0 && CheckedGafs.contains(*Path))
                {
                    const auto& Animations = CheckedGafs.at(*Path);
                    if (size_t(BaseIndex - 1 + PoseCount) > Animations.size())
                    {
                        std::printf("FAIL %s: animation range for %s in %s\n", LevelPath.c_str(), Object.ClassName.c_str(), Path->c_str());
                        bGafsOk = false;
                        continue;
                    }

                    const auto& Frames = Animations[ObjectAnimationIndex(Object, bShadow)].Frames;
                    if (!Frames.empty() && Frames.front().BitmapIndex != 0 && !IsSupportedDrawMode(Frames.front().DrawFlags))
                    {
                        std::printf("FAIL %s: sprite blend for %s in %s\n", LevelPath.c_str(), Object.ClassName.c_str(), Path->c_str());
                        bGafsOk = false;
                    }
                }
            }
        }

        for (const std::string& GafPath : Level->AnimGafs)
        {
            const std::optional<std::vector<uint8_t>> File = Archive.Read(GafPath);
            if (!File || !Gaf::LoadFrames(*File))
            {
                std::printf("FAIL %s: animated map GAF %s\n", LevelPath.c_str(), GafPath.c_str());
                bGafsOk = false;
            }
        }
        if (!bGafsOk)
        {
            ++Failed;
            continue;
        }

        const std::expected<FTerrainCollision, std::string> Terrain = BuildTerrainCollision(*Level);
        if (!Terrain)
        {
            std::printf("FAIL %s: %s\n", LevelPath.c_str(), Terrain.error().c_str());
            ++Failed;
            continue;
        }

        // Patrollers find their path by TYPE Patrol Path-ID (FUN_0042e910); one without a matching path never starts patrolling.
        size_t Patrollers = 0;
        size_t Unrouted = 0;
        bool bBehavioursOk = true;
        for (const FLevelObject& Object : Level->Objects)
        {
            const auto Patrol = std::ranges::find_if(Object.Properties.Children, [](const FLevelPropertyBlock& Block) { return Block.Kind == "TYPE" && Block.Name == "Patrol"; });
            const std::optional<uint32_t> Behaviour = ParseBehaviour(Object.Behaviour);
            if (!Behaviour)
            {
                std::printf("FAIL %s: behaviour '%s' of %s\n", LevelPath.c_str(), Object.Behaviour.c_str(), Object.ClassName.c_str());
                bBehavioursOk = false;
            }

            if (!Behaviour || (*Behaviour & BehaviourMovementMask) != BehaviourPatrol || Patrol == Object.Properties.Children.end())
            {
                continue;
            }

            ++Patrollers;
            const std::optional<float> Id = ParseLevelFloat(Patrol->Find("Path-ID"));
            const auto Path = std::ranges::find(Level->PatrolPaths, Id ? int(*Id) : 0, &FPatrolPath::Id);
            Unrouted += Path == Level->PatrolPaths.end() || Path->Points.empty() ? 1 : 0;
        }

        if (!bBehavioursOk)
        {
            ++Failed;
            continue;
        }

        // Index 0 is the unused placeholder; any other untyped descriptor never reaches a .3DI slot.
        const ptrdiff_t Untyped = std::ranges::count(Level->TileTypes, ETerrain::None) - 1;
        std::printf("%-16s %ux%u, %u layers, %zu blocks, %zu tile types (%td untyped), %zu objects, %zu collision shapes, %u missing tile refs, %zu patrol paths, "
            "%zu patrollers (%zu without a path), \"%s\"\n",
            LevelPath.c_str(), Level->GridWidth, Level->GridHeight, Level->LayerCount, Level->Blocks.size(), Level->TileTypes.size() - 1, Untyped,
            Level->Objects.size(), Terrain->GetShapes().size(), Level->MissingTileRefs, Level->PatrolPaths.size(), Patrollers, Unrouted, Level->Name.c_str());
        ++Loaded;
    }
    std::printf("Levels: %d loaded, %d failed\n", Loaded, Failed);
    return Failed == 0 && Loaded > 0 ? 0 : 1;
}

// Sprites of runtime FX objects belong to no placed object.
constexpr size_t NoObjectIndex = SIZE_MAX;

struct FObjectSprite
{
    size_t SheetIndex = 0;
    size_t ObjectIndex = 0;
    bool bShadow = false;
    // A tank's turret top: the animation after its 24 body poses, one frame per top pose (ObjectDraw at 00431ae0, kind 0x20).
    bool bTop = false;
    Gaf::FAnimationFrame Frame;
    float X = 0.f;
    float Y = 0.f;
    int64_t Depth = 0;
    ESpriteBlend Blend = ESpriteBlend::Opaque;
    float Stretch = 1.f;
};

struct FObjectSprites
{
    std::vector<FSpriteSheet> Sheets;
    std::vector<std::vector<Gaf::FAnimation>> Animations;
    std::vector<FObjectSprite> Sprites;
    std::unordered_map<std::string, size_t> SheetIndices;
};

// Loads a GAF's sheet once and returns its index.
std::optional<size_t> FindOrLoadSheet(FObjectSprites& Sprites, FResourceArchive& Archive, FRenderer& Renderer, const std::string& Path)
{
    auto [Sheet, bInserted] = Sprites.SheetIndices.try_emplace(Path, Sprites.Sheets.size());
    if (bInserted)
    {
        std::optional<FObjectGaf> Gaf = LoadObjectGaf(Archive, Path);
        if (!Gaf)
        {
            Sprites.SheetIndices.erase(Sheet);
            return std::nullopt;
        }

        Sprites.Sheets.push_back(Renderer.CreateSpriteSheet(Gaf->Bitmaps));
        Sprites.Animations.push_back(std::move(Gaf->Animations));
    }

    return Sheet->second;
}

ESpriteBlend FrameBlend(const Gaf::FAnimationFrame& Frame)
{
    const uint32_t DrawMode = Frame.DrawFlags & AnimationBlendMask;
    return DrawMode == AdditiveDrawMode ? ESpriteBlend::Additive : DrawMode == InverseSourceDrawMode ? ESpriteBlend::InverseSourceColor : ESpriteBlend::Opaque;
}

struct FSpriteScale
{
    float X = 1.f;
    float Y = 1.f;
};

// DrawAnimationFrame at 0045eab0: frame event flags 0x40 (X), 0x80 (Y) or 0x100 (both) scale the stretch by the event value in percent.
FSpriteScale FrameScale(const Gaf::FAnimationFrame& Frame, const float Stretch)
{
    constexpr uint32_t ScaleXFlags = 0x140;
    constexpr uint32_t ScaleYFlags = 0x180;
    constexpr float PercentToRatio = 0.01f;
    const auto Scale = [&](const uint32_t Flags)
    {
        return (Frame.EventFlags & Flags) != 0 ? float(double(Frame.EventValue) * Stretch * PercentToRatio) : Stretch;
    };
    return { .X = Scale(ScaleXFlags), .Y = Scale(ScaleYFlags) };
}

// shortcut: initial poses only, use the behaviour update for animation state changes and attachments.
std::expected<FObjectSprites, std::string> CreateObjectSprites(FResourceArchive& Archive, FRenderer& Renderer, const FLevel& Level)
{
    FObjectSprites Result;
    for (size_t ObjectIndex = 0; ObjectIndex < Level.Objects.size(); ++ObjectIndex)
    {
        const FLevelObject& Object = Level.Objects[ObjectIndex];
        for (const bool bShadow : { true, false })
        {
            const std::string& Path = bShadow ? Object.ShadowGafPath : Object.GafPath;
            const int BaseIndex = bShadow ? Object.ShadowIndex : Object.GafIndex;
            if (Path.empty() || BaseIndex == 0)
            {
                continue;
            }

            const std::optional<size_t> SheetIndex = FindOrLoadSheet(Result, Archive, Renderer, Path);
            if (!SheetIndex)
            {
                return std::unexpected("Cannot decode object GAF " + Path);
            }

            const auto& GafAnimations = Result.Animations[*SheetIndex];
            const size_t Index = ObjectAnimationIndex(Object, bShadow);
            if (size_t(BaseIndex - 1) + ObjectPoseCount(Object, bShadow) > GafAnimations.size())
            {
                if (GafAnimations.empty() && Result.Sheets[*SheetIndex].Frames.empty())
                {
                    continue;
                }

                return std::unexpected("Invalid animation index for " + Object.ClassName + " in " + Path);
            }

            for (int Pose = 0; Pose < ObjectPoseCount(Object, bShadow); ++Pose)
            {
                for (const Gaf::FAnimationFrame& Frame : GafAnimations[size_t(BaseIndex - 1 + Pose)].Frames)
                {
                    if (Frame.BitmapIndex != 0 && !IsSupportedDrawMode(Frame.DrawFlags))
                    {
                        return std::unexpected("Unsupported sprite blend for " + Object.ClassName + " in " + Path);
                    }
                }
            }

            const Gaf::FAnimation& Animation = GafAnimations[Index];
            if (Animation.Frames.empty() || Animation.Frames.front().BitmapIndex == 0)
            {
                continue;
            }

            const uint32_t DrawMode = Animation.Frames.front().DrawFlags & AnimationBlendMask;
            if (!IsSupportedDrawMode(Animation.Frames.front().DrawFlags))
            {
                return std::unexpected("Unsupported sprite blend for " + Object.ClassName + " in " + Path);
            }

            const int64_t GroundY = SpriteCoordinate(Object.Y);
            Result.Sprites.push_back({
                .SheetIndex = *SheetIndex,
                .ObjectIndex = ObjectIndex,
                .bShadow = bShadow,
                .Frame = Animation.Frames.front(),
                .X = float(SpriteCoordinate(Object.X)),
                .Y = float(GroundY) - (bShadow ? 0.f : Object.Altitude) - BodyLift(Object, bShadow),
                .Depth = SpriteDepth(GroundY, bShadow, Object.bInGround, Object.bZShadow),
                .Blend = DrawMode == AdditiveDrawMode ? ESpriteBlend::Additive : DrawMode == InverseSourceDrawMode ? ESpriteBlend::InverseSourceColor : ESpriteBlend::Opaque,
            });

            // The top shares the body's depth key and is drawn after it.
            const size_t TopIndex = size_t(Object.GafIndex - 1 + ObjectDirectionCount);
            if (!bShadow && Object.SubType == TankKind && TopIndex < GafAnimations.size() && GafAnimations[TopIndex].Frames.size() == ObjectDirectionCount)
            {
                FObjectSprite Top = Result.Sprites.back();
                Top.bTop = true;
                Top.Frame = GafAnimations[TopIndex].Frames[Index - size_t(Object.GafIndex - 1)];
                Result.Sprites.push_back(Top);
            }
        }
    }

    std::stable_sort(Result.Sprites.begin(), Result.Sprites.end(), [](const FObjectSprite& A, const FObjectSprite& B) { return SpriteDrawsBefore(A.Depth, B.Depth); });
    return Result;
}

void UpdateObjectSprites(FObjectSprites& Sprites, const FLevel& Level)
{
    for (FObjectSprite& Sprite : Sprites.Sprites)
    {
        const FLevelObject& Object = Level.Objects[Sprite.ObjectIndex];
        const Gaf::FAnimation& Animation = Sprites.Animations[Sprite.SheetIndex][ObjectAnimationIndex(Object, Sprite.bShadow)];
        Sprite.Frame = Animation.Frames.empty() ? Gaf::FAnimationFrame{} : Animation.Frames.front();
        if (!Sprite.bShadow && Object.SubType == "heli" && Animation.Frames.size() == ObjectDirectionCount)
        {
            Sprite.Frame = Animation.Frames[size_t(Object.Direction < ObjectDirectionCount ? Object.Direction : 0)];
        }

        if (Sprite.bTop)
        {
            const int Direction = Object.Direction < ObjectDirectionCount ? Object.Direction : 0;
            const size_t Frame = Object.TopPose > 0 ? size_t(Object.TopPose - 1) : size_t(Direction);
            Sprite.Frame = Sprites.Animations[Sprite.SheetIndex][size_t(Object.GafIndex - 1 + ObjectDirectionCount)].Frames[Frame];
        }

        const uint32_t DrawMode = Sprite.Frame.DrawFlags & AnimationBlendMask;
        Sprite.Blend = DrawMode == AdditiveDrawMode ? ESpriteBlend::Additive : DrawMode == InverseSourceDrawMode ? ESpriteBlend::InverseSourceColor : ESpriteBlend::Opaque;
        const int64_t GroundY = SpriteCoordinate(Object.Y);
        Sprite.X = float(SpriteCoordinate(Object.X));
        Sprite.Y = float(GroundY) - (Sprite.bShadow ? 0.f : Object.Altitude) - BodyLift(Object, Sprite.bShadow);
        Sprite.Depth = SpriteDepth(GroundY, Sprite.bShadow, Object.bInGround, Object.bZShadow);
    }

    std::stable_sort(Sprites.Sprites.begin(), Sprites.Sprites.end(), [](const FObjectSprite& A, const FObjectSprite& B) { return SpriteDrawsBefore(A.Depth, B.Depth); });
}

uint32_t ClockMilliseconds()
{
    const auto Elapsed = std::chrono::steady_clock::now().time_since_epoch();
    return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(Elapsed).count());
}
}

int main(const int ArgCount, char** const Args)
{
    std::optional<std::filesystem::path> DataPath;
    std::string LevelPath = "underground\\t1u1";
    // Development aid: --start X,Y spawns the player at that world position.
    std::optional<std::pair<float, float>> Start;
    bool bVerifyGafs = false;
    bool bVerifyLevels = false;
    bool bVerifyGame = false;
    // Holds the trigger down, for checking shots and explosions without a keyboard.
    bool bAutoFire = false;
    for (int Index = 1; Index < ArgCount; ++Index)
    {
        if (std::strcmp(Args[Index], "--verify-gaf") == 0)
        {
            bVerifyGafs = true;
        }
        else if (std::strcmp(Args[Index], "--verify-levels") == 0)
        {
            bVerifyLevels = true;
        }
        else if (std::strcmp(Args[Index], "--verify-game") == 0)
        {
            bVerifyGame = true;
        }
        else if (std::strcmp(Args[Index], "--auto-fire") == 0)
        {
            bAutoFire = true;
        }
        else if (std::strcmp(Args[Index], "--level") == 0 && Index + 1 < ArgCount)
        {
            LevelPath = Args[++Index];
        }
        else if (float X = 0.f, Y = 0.f; std::strcmp(Args[Index], "--start") == 0 && Index + 1 < ArgCount && std::sscanf(Args[Index + 1], "%f,%f", &X, &Y) == 2)
        {
            Start = { X, Y };
            ++Index;
        }
        else
        {
            DataPath = Args[Index];
        }
    }

    if (!DataPath)
    {
        const auto Directory = ExecutableDirectory();
        if (!Directory)
        {
            std::fprintf(stderr, "%s\n", Directory.error().c_str());
            return 1;
        }

        DataPath = *Directory / "vz-data.rsc";
    }
    else
    {
        std::error_code Error;
        if (std::filesystem::is_directory(*DataPath, Error))
        {
            *DataPath /= "vz-data.rsc";
        }
    }

    FResourceArchive Archive;
    if (!Archive.Open(*DataPath))
    {
        std::fprintf(stderr, "Cannot open %s\n", DataPath->string().c_str());
        return 1;
    }
    const std::optional<std::vector<uint8_t>> Config = Archive.Read("VZCONF.TXT");
    if (!Config || Config->empty() || (*Config)[0] != '#')
    {
        std::fprintf(stderr, "vz-data.rsc is unreadable: VZCONF.TXT missing or corrupt\n");
        return 1;
    }
    std::printf("vz-data.rsc: %zu entries\n", Archive.GetEntryCount());
    std::fflush(stdout);
    if (bVerifyGafs)
    {
        return VerifyGafs(Archive);
    }
    if (bVerifyLevels)
    {
        return VerifyLevels(Archive);
    }
    if (bVerifyGame)
    {
        const bool bFrameLoop = VerifyFrameLoop();
        const std::expected<void, std::string> Collision = VerifyTerrainCollision();
        const std::expected<void, std::string> Movement = VerifyPlayerMovement();
        const std::expected<void, std::string> Paths = VerifyPathSearch();
        std::printf("Path search: %s\n", Paths ? "passed" : Paths.error().c_str());
        const std::expected<std::string, std::string> Demo = VerifyDemoReplay(Archive);
        std::printf("Demo replay: %s\n", Demo ? Demo->c_str() : Demo.error().c_str());
        const std::expected<std::string, std::string> Combat = VerifyCombat(Archive);
        std::printf("Combat: %s\n", Combat ? Combat->c_str() : Combat.error().c_str());
        const std::expected<std::string, std::string> Patrols = VerifyPatrols(Archive);
        std::printf("Patrols: %s\n", Patrols ? Patrols->c_str() : Patrols.error().c_str());
        std::printf("Frame loop: %s, terrain collision: %s, player movement: %s\n", bFrameLoop ? "passed" : "failed",
            Collision ? "passed" : Collision.error().c_str(), Movement ? "passed" : Movement.error().c_str());
        return bFrameLoop && Collision && Movement && Paths && Demo && Combat && Patrols ? 0 : 1;
    }

    std::expected<FLevel, std::string> Level = LoadLevel(Archive, LevelPath);
    if (!Level)
    {
        std::fprintf(stderr, "Cannot load level: %s\n", Level.error().c_str());
        return 1;
    }

    if (!glfwInit())
    {
        std::fprintf(stderr, "glfwInit failed\n");
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* const Window = glfwCreateWindow(static_cast<int>(ScreenWidth) * 2, static_cast<int>(ScreenHeight) * 2, "Vyruz", nullptr, nullptr);
    if (!Window)
    {
        std::fprintf(stderr, "glfwCreateWindow failed\n");
        return 1;
    }

    FRenderer Renderer;
    Renderer.Init(Window);

    // Level view: MAP_ layers drawn bottom-up with the placed objects. The player drives with the original movement code; levels
    // without a player fall back to scrolling with the arrow keys.
    const FSpriteSheet Blocks = Renderer.CreateSpriteSheet(Level->Blocks);
    std::expected<FObjectSprites, std::string> Objects = CreateObjectSprites(Archive, Renderer, *Level);
    if (!Objects)
    {
        std::fprintf(stderr, "%s\n", Objects.error().c_str());
        Renderer.Shutdown();
        glfwDestroyWindow(Window);
        glfwTerminate();
        return 1;
    }

    constexpr double ScrollSpeed = 600.0;
    const float MapWidth = float(Level->GridWidth * BlockWidth);
    const float MapHeight = float(Level->GridHeight * BlockHeight);
    const auto PlayerObject = std::ranges::find_if(Level->Objects, [](const FLevelObject& Object) { return Object.Behaviour == "Player"; });
    std::optional<FMovementClass> PlayerClass;
    std::optional<FPlayerState> Player;
    std::optional<FTerrainCollision> Terrain;
    const GLFWvidmode* const DisplayMode = glfwGetVideoMode(glfwGetPrimaryMonitor());
    const uint32_t RefreshMillihertz = DisplayMode && DisplayMode->refreshRate > 0 ? uint32_t(DisplayMode->refreshRate) * MillihertzPerHertz : 0;
    const uint32_t FramePeriod = FramePeriodMilliseconds(RefreshMillihertz);
    std::optional<FCombat> Combat;
    // The smaller side of the player's current frame, which AnimUpdateBounds makes the body's bounds.
    float PlayerSpriteSize = 0.f;
    std::vector<FObjectBody> Bodies = CreateObjectBodies(*Level);
    const size_t PlayerIndex = size_t(PlayerObject - Level->Objects.begin());
    std::vector<FPickup> Pickups = CreatePickups(*Level);
    FPlayerInventory Inventory = PlayerObject != Level->Objects.end() ? CreateInventory(*PlayerObject) : FPlayerInventory{};
    if (PlayerObject != Level->Objects.end())
    {
        std::expected<FMovementClass, std::string> Class = ReadMovementClass(*PlayerObject);
        std::expected<FTerrainCollision, std::string> Collision = BuildTerrainCollision(*Level);
        if (Class && Collision)
        {
            PlayerClass = *Class;
            if (Start)
            {
                PlayerObject->X = Start->first;
                PlayerObject->Y = Start->second;
            }

            Player = SpawnPlayer(*PlayerObject, *Class);
            Terrain = std::move(*Collision);
            if (std::expected<FCombat, std::string> Weapons = CreateCombat(*Level, *PlayerObject, Archive, Bodies))
            {
                Combat = std::move(*Weapons);
                for (const FEffectClass& Effect : Combat->EffectClasses)
                {
                    if (!FindOrLoadSheet(*Objects, Archive, Renderer, Effect.GafPath))
                    {
                        std::fprintf(stderr, "Cannot decode FX GAF %s\n", Effect.GafPath.c_str());
                    }
                }
            }
            else
            {
                std::fprintf(stderr, "Player cannot fire: %s\n", Weapons.error().c_str());
            }
        }
        else
        {
            std::fprintf(stderr, "Player is not controllable: %s\n", !Class ? Class.error().c_str() : Collision.error().c_str());
        }
    }

    double CameraX = PlayerObject != Level->Objects.end() ? PlayerObject->X - ScreenWidth * 0.5 : 0.0;
    double CameraY = PlayerObject != Level->Objects.end() ? PlayerObject->Y - ScreenHeight * 0.5 : 0.0;
    std::printf("%s: %s, %zu objects, %zu sprite parts\n", LevelPath.c_str(), Level->Name.c_str(), Level->Objects.size(), Objects->Sprites.size());
    std::fflush(stdout);

    FFrameClock Clock(FramePeriod);
    Clock.Poll(ClockMilliseconds());
    // ponytail: frame rate in the window title until the HUD can draw text.
    uint32_t FpsWindowStart = ClockMilliseconds();
    uint32_t FpsFrames = 0;
    while (!glfwWindowShouldClose(Window))
    {
        glfwPollEvents();
        // ponytail: Esc quits straight away until the original pause menu is ported.
        if (glfwGetKey(Window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        {
            glfwSetWindowShouldClose(Window, GLFW_TRUE);
        }
        uint32_t RemainingMilliseconds = Clock.Poll(ClockMilliseconds());
        while (RemainingMilliseconds != 0)
        {
            const uint32_t StepMilliseconds = NextGameStep(RemainingMilliseconds, FramePeriod, false);
            const auto Pressed = [Window](const int Key) { return glfwGetKey(Window, Key) == GLFW_PRESS; };
            if (Player)
            {
                const FPlayerCommand Command = PlayerCommandFromKeys(Pressed(GLFW_KEY_UP), Pressed(GLFW_KEY_DOWN), Pressed(GLFW_KEY_LEFT), Pressed(GLFW_KEY_RIGHT),
                    Pressed(GLFW_KEY_A), Pressed(GLFW_KEY_D));
                StepPlayer(*Player, *PlayerClass, Command, StepMilliseconds, *Terrain, *Level, Bodies);
                StepObjectBodies(Bodies, StepMilliseconds, *Terrain, *Level);
                StepActors(Combat ? &*Combat : nullptr, Bodies, StepMilliseconds, *Terrain, *Level, &*Player);
                if (Combat)
                {
                    const float MoneyBefore = Inventory.Money;
                    Combat->Clock += StepMilliseconds;
                    const float EnergyBefore = Inventory.Energy;
                    const float ShieldBefore = Inventory.Shield;
                    const bool bFire = bAutoFire || Pressed(GLFW_KEY_SPACE) || Pressed(GLFW_KEY_LEFT_CONTROL);
                    StepPlayerWeapon(*Combat, bFire, *Player, *PlayerObject, PlayerIndex, PlayerSpriteSize, Inventory);
                    StepTurrets(*Combat, StepMilliseconds, *Terrain, *Level, Bodies, *Player);
                    StepProjectiles(*Combat, StepMilliseconds, *Terrain, *Level, Bodies,
                        { .State = *Player, .Radius = PlayerClass->Radius, .Inventory = Inventory, .ObjectIndex = PlayerIndex });
                    if (Inventory.Money != MoneyBefore || Inventory.Energy != EnergyBefore || Inventory.Shield != ShieldBefore)
                    {
                        // ponytail: no HUD or death sequence yet; the totals go to the console.
                        std::printf("Energy %.0f, shield %.0f, money %.0f\n", Inventory.Energy, Inventory.Shield, Inventory.Money);
                        std::fflush(stdout);
                    }
                }
                if (CollectPickups(*Player, *PlayerClass, Inventory, Pickups, *Level) != 0)
                {
                    std::printf("Energy %.0f, shield %.0f, money %.0f\n", Inventory.Energy, Inventory.Shield, Inventory.Money);
                    std::fflush(stdout);
                }
            }
            else
            {
                const double ScrollStep = StepMilliseconds * ScrollSpeed / MillisecondsPerSecond;
                CameraX += (Pressed(GLFW_KEY_RIGHT) - Pressed(GLFW_KEY_LEFT)) * ScrollStep;
                CameraY += (Pressed(GLFW_KEY_DOWN) - Pressed(GLFW_KEY_UP)) * ScrollStep;
            }
        }

        if (Player)
        {
            for (FObjectBody& Body : Bodies)
            {
                FLevelObject& Object = Level->Objects[Body.ObjectIndex];
                Object.X = Body.State.Position.X;
                Object.Y = Body.State.Position.Y;
                // A turret draws its rotating top; the fixed base (animation 25) is not drawn yet.
                if (Body.Turret)
                {
                    Object.Direction = Body.Turret->Direction;
                }
                else if (Body.Actor)
                {
                    Object.Direction = Body.State.Direction;
                    Object.TopPose = Body.Actor->bTwoPieces ? Body.Actor->TopPose : 0;
                    // The tilt counter moves once per drawn frame, as the original updates it while drawing.
                    if (Object.SubType == "heli")
                    {
                        Object.Tilt = SelectHeliTilt(*Body.Actor, Body.State.SpeedMagnitude);
                    }
                }
            }

            PlayerObject->X = Player->Position.X;
            PlayerObject->Y = Player->Position.Y;
            PlayerObject->Direction = (Player->Pose - 1) / (PlayerPoseCount / ObjectDirectionCount);
            // Match the sprite's rounding so fractional movement cannot shift the ship against the camera.
            const int ViewWidth = int(ScreenWidth);
            const int ViewHeight = int(ScreenHeight);
            CameraX = std::clamp(int(SpriteCoordinate(Player->Position.X)) - ViewWidth / 2, 0, std::max(0, int(Level->WorldExtentX) - ViewWidth));
            CameraY = std::clamp(int(SpriteCoordinate(Player->Position.Y)) - ViewHeight / 2, 0, std::max(0, int(Level->WorldExtentY) - ViewHeight));
        }

        UpdateObjectSprites(*Objects, *Level);
        if (Player)
        {
            // The player's 48 poses are the frames of the rot animation that follows its 24 direction animations; odd poses match them.
            const size_t RotAnimation = size_t(PlayerObject->GafIndex - 1 + ObjectDirectionCount);
            for (FObjectSprite& Sprite : Objects->Sprites)
            {
                const std::vector<Gaf::FAnimation>& Animations = Objects->Animations[Sprite.SheetIndex];
                if (Sprite.ObjectIndex == PlayerIndex && !Sprite.bShadow && RotAnimation < Animations.size() &&
                    Animations[RotAnimation].Frames.size() == size_t(PlayerPoseCount))
                {
                    Sprite.Frame = Animations[RotAnimation].Frames[size_t(Player->Pose - 1)];
                    const std::vector<FSpriteFrame>& Frames = Objects->Sheets[Sprite.SheetIndex].Frames;
                    if (Sprite.Frame.BitmapIndex != 0 && Sprite.Frame.BitmapIndex <= Frames.size())
                    {
                        const FSpriteFrame& Bitmap = Frames[Sprite.Frame.BitmapIndex - 1];
                        PlayerSpriteSize = float(std::min(Bitmap.Width, Bitmap.Height));
                    }
                }
            }
        }
        CameraX = std::clamp(CameraX, 0.0, std::max(0.0, double(MapWidth - ScreenWidth)));
        CameraY = std::clamp(CameraY, 0.0, std::max(0.0, double(MapHeight - ScreenHeight)));

        if (!Renderer.BeginFrame())
        {
            glfwWaitEventsTimeout(0.1);
            continue;
        }

        const int OriginX = int(std::floor(CameraX));
        const int OriginY = int(std::floor(CameraY));
        const uint32_t FirstX = uint32_t(OriginX) / BlockWidth;
        const uint32_t FirstY = uint32_t(OriginY) / BlockHeight;
        const uint32_t EndX = std::min(Level->GridWidth, (uint32_t(OriginX) + uint32_t(ScreenWidth) - 1) / BlockWidth + 1);
        const uint32_t EndY = std::min(Level->GridHeight, (uint32_t(OriginY) + uint32_t(ScreenHeight) - 1) / BlockHeight + 1);
        // Runtime FX join the placed objects in the original depth order.
        std::vector<FObjectSprite> Sprites = Objects->Sprites;
        if (Combat)
        {
            for (const FEffect& Effect : Combat->Effects)
            {
                const FEffectClass& Class = Combat->EffectClasses[Effect.ClassIndex];
                const auto Sheet = Objects->SheetIndices.find(Class.GafPath);
                if (Effect.Frame == 0 || Sheet == Objects->SheetIndices.end())
                {
                    continue;
                }

                const Gaf::FAnimationFrame& Frame = Objects->Animations[Sheet->second][size_t(Class.GafIndex - 1)].Frames[Effect.Frame - 1];
                const int64_t GroundY = SpriteCoordinate(Effect.Position.Y);
                Sprites.push_back({
                    .SheetIndex = Sheet->second,
                    .ObjectIndex = NoObjectIndex,
                    .Frame = Frame,
                    .X = float(SpriteCoordinate(Effect.Position.X)),
                    .Y = float(GroundY) - Effect.Altitude,
                    .Depth = SpriteDepth(GroundY, false, false, false),
                    .Blend = FrameBlend(Frame),
                    .Stretch = Class.Stretch,
                });
            }

            std::stable_sort(Sprites.begin(), Sprites.end(), [](const FObjectSprite& A, const FObjectSprite& B) { return SpriteDrawsBefore(A.Depth, B.Depth); });
        }

        const auto DrawObject = [&](const FObjectSprite& Sprite)
        {
            const Gaf::FAnimationFrame& Frame = Sprite.Frame;
            if (Frame.BitmapIndex == 0 || (Sprite.ObjectIndex != NoObjectIndex && Level->Objects[Sprite.ObjectIndex].bRemoved))
            {
                return;
            }

            // The hotspot offset is scaled and truncated like the original's __ftol.
            const FSpriteScale Scale = FrameScale(Frame, Sprite.Stretch);
            const float OffsetX = Scale.X != 1.f ? std::trunc(float(Frame.OffsetX) * Scale.X) : float(Frame.OffsetX);
            const float OffsetY = Scale.Y != 1.f ? std::trunc(float(Frame.OffsetY) * Scale.Y) : float(Frame.OffsetY);
            Renderer.DrawSprite(Objects->Sheets[Sprite.SheetIndex], Frame.BitmapIndex - 1, Sprite.X + OffsetX - OriginX, Sprite.Y + OffsetY - OriginY,
                Frame.bMirrorX, Frame.bMirrorY, Sprite.Blend, Scale.X, Scale.Y);
        };
        const auto DrawMapRow = [&](const int64_t Row, const std::optional<int32_t> Depth)
        {
            if (Row < FirstY || Row >= EndY)
            {
                return;
            }

            // LevelDrawTerrainRow at 0041fd40 leaves the first two planes in the background cache.
            for (uint32_t Layer = Depth ? FirstOccludingMapLayer : 0; Layer < Level->LayerCount; ++Layer)
            {
                const uint32_t* const Cells = Level->Map.data() + size_t{ Layer } * Level->GridWidth * Level->GridHeight;
                for (uint32_t X = FirstX; X < EndX; ++X)
                {
                    const uint32_t Block = Cells[size_t(Row) * Level->GridWidth + X];
                    if (Block != 0 && (!Depth || Level->BlockDepths[Block - 1] == *Depth))
                    {
                        Renderer.DrawSprite(Blocks, Block - 1, float(int(X * BlockWidth) - OriginX), float(Row * BlockHeight - OriginY));
                    }
                }
            }
        };
        std::array<size_t, SpriteDepthLayerCount> NextSprite{};
        const auto FlushSprites = [&](const int64_t Threshold, const int64_t Layer)
        {
            size_t& Next = NextSprite[size_t(Layer)];
            for (; Next < Sprites.size(); ++Next)
            {
                const FObjectSprite& Sprite = Sprites[Next];
                if ((Sprite.Depth & SpriteDepthLayerMask) != Layer)
                {
                    continue;
                }

                if (Sprite.Depth >= Threshold)
                {
                    break;
                }

                DrawObject(Sprite);
            }
        };
        for (uint32_t Y = FirstY; Y < EndY; ++Y)
        {
            DrawMapRow(Y, std::nullopt);
        }

        constexpr int64_t FinalThreshold = std::numeric_limits<int64_t>::max();
        FlushSprites(FinalThreshold, InGroundDepthLayer);
        // LevelCompositeSprites at 00420070 interleaves five terrain depths with shadows and bodies.
        for (int64_t Row = FirstY; Row < int64_t(EndY) + MaxTerrainDepth; ++Row)
        {
            for (int32_t Depth = MaxTerrainDepth; Depth >= 0; --Depth)
            {
                DrawMapRow(Row - Depth, Depth);
            }

            const int64_t Threshold = (Row + 1) * BlockHeight * SpriteDepthLayerCount;
            for (const int64_t Layer : { ShadowDepthLayer, MiddleDepthLayer, BodyDepthLayer })
            {
                FlushSprites(Threshold, Layer);
            }
        }

        for (const int64_t Layer : { ShadowDepthLayer, MiddleDepthLayer, BodyDepthLayer })
        {
            FlushSprites(FinalThreshold, Layer);
        }

        Renderer.EndFrame();
        ++FpsFrames;
        constexpr uint32_t FpsWindowMilliseconds = MillisecondsPerSecond;
        if (const uint32_t Now = ClockMilliseconds(); Now - FpsWindowStart >= FpsWindowMilliseconds)
        {
            const std::string Title = std::format("Vyruz - {:.0f} fps", FpsFrames * double(MillisecondsPerSecond) / (Now - FpsWindowStart));
            glfwSetWindowTitle(Window, Title.c_str());
            FpsWindowStart = Now;
            FpsFrames = 0;
        }
    }

    Renderer.Shutdown();
    glfwDestroyWindow(Window);
    glfwTerminate();
    return 0;
}
