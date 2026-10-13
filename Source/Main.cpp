#include "Game/Collision.h"
#include "Game/Engine.h"
#include "Game/Level.h"
#include "Game/Loop.h"
#include "Game/Panel.h"
#include "Pg/File.h"
#include "Pg/Font.h"
#include "Pg/Gaf.h"
#include "Pg/ResourceArchive.h"
#include "Render/Renderer.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <span>
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
constexpr std::string_view TurretKind = "turret";

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
    const int Offset = PoseCount > 1 && Object.SubType != "heli" ? Direction / (ObjectDirectionCount / PoseCount) : 0;
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
    // A Use-delta-packing lift or gate stores later frames as changes over the first, which ObjectDraw (00431ae0, FUN_00433710) draws
    // underneath first.
    bool bDeltaBase = false;
    // A turret's fixed base: the animation after its 24 top poses, drawn under the top (ObjectDraw, kind 0x1e).
    bool bTurretBase = false;
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

// An actor's part of ObjectDraw (00431ae0): the walk it plays and the frames it last chose. Animation indices are 0-based into the GAF.
struct FActorAnimation
{
    // A walker's 24 or a spider's 4 walking animations, which only the object passes advance.
    std::vector<FAnimationPlayback> Walks;
    // Object +0x238: the pose a walker last walked in.
    int PreviousPose = 0;
    bool bInView = false;
    size_t BodyAnimation = 0;
    // 1-based; zero draws nothing.
    uint32_t BodyFrame = 0;
    size_t ShadowAnimation = 0;
    uint32_t ShadowFrame = 0;
};

// Animation indices AnimInitializeGraphics (00417b30) starts at fixed positions rather than from the class's GAF-index.
constexpr size_t CopRotAnimation = 24;
constexpr size_t SpiderRotAnimation = 4;
// A spider walks only at every sixth of its 24 poses: the four diagonals.
constexpr int SpiderPosesPerWalk = 6;

const std::vector<Gaf::FAnimation>* FindAnimations(const FObjectSprites& Sprites, const std::string& Path)
{
    const auto Sheet = Sprites.SheetIndices.find(Path);
    return Sheet == Sprites.SheetIndices.end() ? nullptr : &Sprites.Animations[Sheet->second];
}

std::vector<std::optional<FActorAnimation>> CreateActorAnimations(const std::span<const FObjectBody> Bodies, const FLevel& Level, const FObjectSprites& Sprites)
{
    std::vector<std::optional<FActorAnimation>> Result(Level.Objects.size());
    for (const FObjectBody& Body : Bodies)
    {
        const FLevelObject& Object = Level.Objects[Body.ObjectIndex];
        const std::vector<Gaf::FAnimation>* const Animations = FindAnimations(Sprites, Object.GafPath);
        if (!Body.Actor || !Animations || Object.GafIndex < 1)
        {
            continue;
        }

        FActorAnimation& Actor = Result[Body.ObjectIndex].emplace(FActorAnimation{ .PreviousPose = Body.State.Pose });
        const size_t WalkCount = Object.SubType == "walker" ? size_t(ObjectDirectionCount) : Object.SubType == "spider" ? size_t(FourWayDirectionCount) : 0;
        for (size_t Walk = 0; Walk < WalkCount && size_t(Object.GafIndex - 1) + Walk < Animations->size(); ++Walk)
        {
            Actor.Walks.push_back(StartAnimation((*Animations)[size_t(Object.GafIndex - 1) + Walk]));
        }
    }

    return Result;
}

// The actor kinds of ObjectDraw (00431ae0), which runs for every object in the view's cells once per step in the update pass and again in the frame's
// draw pass; both advance walks by the step. Cops, jeeps, tanks and helis show their pose as a frame of a fixed animation; jeep and tank
// animations have a single looping frame, so playing them changes nothing.
void AnimateActors(std::vector<std::optional<FActorAnimation>>& Actors, const std::span<FObjectBody> Bodies, const FLevel& Level, const FObjectSprites& Sprites,
    const FViewCells View, const int32_t StepMilliseconds)
{
    for (std::optional<FActorAnimation>& Actor : Actors)
    {
        if (Actor.has_value())
        {
            Actor.value().bInView = false;
        }
    }

    for (FObjectBody& Body : Bodies)
    {
        std::optional<FActorAnimation>& Actor = Actors[Body.ObjectIndex];
        const FLevelObject& Object = Level.Objects[Body.ObjectIndex];
        const std::vector<Gaf::FAnimation>* const Animations = FindAnimations(Sprites, Object.GafPath);
        if (!Actor || !Body.Actor || !Animations || Object.bRemoved || Body.State.Pose < 1 || !View.Contains(Body.State.Position))
        {
            continue;
        }

        const int Pose = Body.State.Pose;
        const size_t Base = size_t(Object.GafIndex - 1);
        const bool bForward = Body.Actor->Command.Thrust == EPlayerThrust::Forward;
        Actor->bInView = true;
        Actor->ShadowAnimation = size_t(std::max(Object.ShadowIndex - 1, 0));
        Actor->ShadowFrame = uint32_t(Pose);
        Actor->BodyFrame = uint32_t(Pose);
        if (Object.SubType == "cop")
        {
            Actor->BodyAnimation = CopRotAnimation;
        }
        else if (Object.SubType == "jeep" || Object.SubType == TankKind)
        {
            Actor->BodyAnimation = (Object.SubType == TankKind ? 0 : Base) + size_t(Pose - 1);
            Actor->BodyFrame = 1;
        }
        else if (Object.SubType == "heli")
        {
            Actor->BodyAnimation = Base + size_t(SelectHeliTilt(*Body.Actor, Body.State.SpeedMagnitude));
        }
        else if (Object.SubType == "walker" && size_t(Pose) <= Actor->Walks.size())
        {
            // A new pose carries the walk on from the frame the previous pose's walk had reached.
            const bool bTurned = Pose != Actor->PreviousPose;
            FAnimationPlayback& Walk = Actor->Walks[size_t(Pose - 1)];
            const Gaf::FAnimation& Animation = (*Animations)[Base + size_t(Pose - 1)];
            if (bTurned)
            {
                const bool bHadWalk = Actor->PreviousPose >= 1 && size_t(Actor->PreviousPose) <= Actor->Walks.size();
                const FAnimationPlayback Previous = bHadWalk ? Actor->Walks[size_t(Actor->PreviousPose - 1)] : Walk;
                ShowFrame(Walk, Animation, Previous.bEnded ? 0 : Previous.Frame);
                Actor->PreviousPose = Pose;
            }

            if (bForward || bTurned)
            {
                UpdateAnimation(Walk, Animation, StepMilliseconds);
            }

            Actor->BodyAnimation = Base + size_t(Pose - 1);
            Actor->BodyFrame = Walk.bEnded ? 0 : Walk.Frame;
            Actor->ShadowFrame = 1;
        }
        else if (Object.SubType == "spider" && size_t((Pose - 1) / SpiderPosesPerWalk) < Actor->Walks.size())
        {
            // Between diagonals, or standing on one, a spider shows its turning pose and its walk restarts.
            const size_t Diagonal = size_t((Pose - 1) / SpiderPosesPerWalk);
            FAnimationPlayback& Walk = Actor->Walks[Diagonal];
            const Gaf::FAnimation& Animation = (*Animations)[Base + Diagonal];
            Actor->ShadowAnimation += Diagonal;
            Actor->ShadowFrame = 1;
            if ((Pose - 1) % SpiderPosesPerWalk == 0 && bForward)
            {
                UpdateAnimation(Walk, Animation, StepMilliseconds);
                Actor->BodyAnimation = Base + Diagonal;
                Actor->BodyFrame = Walk.bEnded ? 0 : Walk.Frame;
            }
            else
            {
                ShowFrame(Walk, Animation, 1);
                Actor->BodyAnimation = SpiderRotAnimation;
            }
        }
    }
}

// EngineVisitViewObjects at 00433b30 visits from one column left of the view and two rows above it, 13 columns and 20 rows, clamped to the grid.
FViewCells ViewCellsAround(const int CameraX, const int CameraY, const FLevel& Level)
{
    constexpr int ExtraColumns = 3;
    constexpr int ExtraRows = 5;
    constexpr int LeadingColumns = 1;
    constexpr int LeadingRows = 2;
    const int Left = std::max(CameraX / int(BlockWidth) - LeadingColumns, 0);
    const int Top = std::max(CameraY / int(BlockHeight) - LeadingRows, 0);
    return {
        .Left = Left,
        .Top = Top,
        .Right = std::min(Left + int(ScreenWidth) / int(BlockWidth) + ExtraColumns, int(Level.GridWidth)),
        .Bottom = std::min(Top + int(ScreenHeight) / int(BlockHeight) + ExtraRows, int(Level.GridHeight)),
    };
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
// The sprites of one object, appended unsorted.
std::expected<void, std::string> AddObjectSprites(FObjectSprites& Result, FResourceArchive& Archive, FRenderer& Renderer, const FLevel& Level, const size_t ObjectIndex)
{
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

            const bool bDoor = Object.SubType == "lift" || Object.SubType == "gate";
            if (!bShadow && bDoor && ParseLevelFloat(Object.Properties.Find("Use-delta-packing")).value_or(0.f) != 0.f)
            {
                FObjectSprite Base = Result.Sprites.back();
                Base.bDeltaBase = true;
                Result.Sprites.insert(Result.Sprites.end() - 1, Base);
            }

            // The top shares the body's depth key and is drawn after it.
            const size_t TopIndex = size_t(Object.GafIndex - 1 + ObjectDirectionCount);
            if (!bShadow && Object.SubType == TankKind && TopIndex < GafAnimations.size() && GafAnimations[TopIndex].Frames.size() == ObjectDirectionCount)
            {
                FObjectSprite Top = Result.Sprites.back();
                Top.bTop = true;
                Top.Frame = GafAnimations[TopIndex].Frames[Index - size_t(Object.GafIndex - 1)];
                Result.Sprites.push_back(Top);
            }

            if (!bShadow && Object.SubType == TurretKind && TopIndex < GafAnimations.size() && !GafAnimations[TopIndex].Frames.empty())
            {
                FObjectSprite Base = Result.Sprites.back();
                Base.bTurretBase = true;
                Base.Frame = GafAnimations[TopIndex].Frames.front();
                Result.Sprites.insert(Result.Sprites.end() - 1, Base);
            }
        }
    }

    return {};
}

std::expected<FObjectSprites, std::string> CreateObjectSprites(FResourceArchive& Archive, FRenderer& Renderer, const FLevel& Level)
{
    FObjectSprites Result;
    for (size_t ObjectIndex = 0; ObjectIndex < Level.Objects.size(); ++ObjectIndex)
    {
        if (std::expected<void, std::string> Added = AddObjectSprites(Result, Archive, Renderer, Level, ObjectIndex); !Added)
        {
            return std::unexpected(Added.error());
        }
    }

    std::stable_sort(Result.Sprites.begin(), Result.Sprites.end(), [](const FObjectSprite& A, const FObjectSprite& B) { return SpriteDrawsBefore(A.Depth, B.Depth); });
    return Result;
}

void UpdateObjectSprites(FObjectSprites& Sprites, const FLevel& Level, const std::vector<std::optional<FActorAnimation>>& Actors, const std::span<const FDoor> Doors)
{
    std::vector<const FDoor*> DoorOf(Level.Objects.size(), nullptr);
    for (const FDoor& Door : Doors)
    {
        DoorOf[Door.ObjectIndex] = &Door;
    }

    for (FObjectSprite& Sprite : Sprites.Sprites)
    {
        const FLevelObject& Object = Level.Objects[Sprite.ObjectIndex];
        const std::vector<Gaf::FAnimation>& Animations = Sprites.Animations[Sprite.SheetIndex];
        const Gaf::FAnimation& Animation = Animations[ObjectAnimationIndex(Object, Sprite.bShadow)];
        Sprite.Frame = Animation.Frames.empty() ? Gaf::FAnimationFrame{} : Animation.Frames.front();
        // An actor is drawn only from the view's cells, with the frames its last object pass chose.
        if (const std::optional<FActorAnimation>& Actor = Actors[Sprite.ObjectIndex]; Actor && !Sprite.bTop)
        {
            const size_t Index = Sprite.bShadow ? Actor->ShadowAnimation : Actor->BodyAnimation;
            const uint32_t Frame = Sprite.bShadow ? Actor->ShadowFrame : Actor->BodyFrame;
            const bool bShown = Actor->bInView && Index < Animations.size() && Frame >= 1 && Frame <= Animations[Index].Frames.size();
            Sprite.Frame = bShown ? Animations[Index].Frames[Frame - 1] : Gaf::FAnimationFrame{};
        }

        // A gate or lift shows the animation its state machine plays.
        if (const FDoor* const Door = DoorOf[Sprite.ObjectIndex]; Door && !Sprite.bShadow)
        {
            const size_t Index = size_t(Object.GafIndex - 1) + Door->Animation;
            const uint32_t Frame = Sprite.bDeltaBase ? 1 : Door->Playback.bEnded ? 0 : Door->Playback.Frame;
            const bool bShown = Index < Animations.size() && Frame >= 1 && Frame <= Animations[Index].Frames.size();
            Sprite.Frame = bShown ? Animations[Index].Frames[Frame - 1] : Gaf::FAnimationFrame{};
        }

        if (Sprite.bTop)
        {
            const int Direction = Object.Direction < ObjectDirectionCount ? Object.Direction : 0;
            const size_t Frame = Object.TopPose > 0 ? size_t(Object.TopPose - 1) : size_t(Direction);
            Sprite.Frame = Sprites.Animations[Sprite.SheetIndex][size_t(Object.GafIndex - 1 + ObjectDirectionCount)].Frames[Frame];
        }

        if (Sprite.bTurretBase)
        {
            Sprite.Frame = Sprites.Animations[Sprite.SheetIndex][size_t(Object.GafIndex - 1 + ObjectDirectionCount)].Frames.front();
        }
        // A wreck has lost its upper half: a tank's top or a turret's gun.
        else if (Object.bWrecked && !Sprite.bShadow && (Sprite.bTop || Object.SubType == TurretKind))
        {
            Sprite.Frame = {};
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

// What LoopLevels (004467d0) keeps of the player object for the next level: everything but its position and velocity.
struct FCarriedPlayer
{
    FLevelObject Object;
    FPlayerState State;
    FPlayerInventory Inventory;
    FWeaponState Weapon;
    uint32_t Clock = 1;
};

// One level of LoopLevels: runs until the window closes or the player takes a lift, and then returns the next level's path with the player
// in Carried.
std::expected<std::optional<std::string>, std::string> RunLevel(GLFWwindow* const Window, FRenderer& Renderer, FResourceArchive& Archive,
    const std::string& LevelPath, const std::optional<std::pair<float, float>> Start, const bool bAutoFire, std::optional<FCarriedPlayer>& Carried,
    const std::span<const std::string> DebugLevels, FPanel* const Panel)
{
    std::expected<FLevel, std::string> Level = LoadLevel(Archive, LevelPath);
    if (!Level)
    {
        return std::unexpected(std::format("Cannot load level: {}", Level.error()));
    }

    const auto PlayerObject = std::ranges::find_if(Level->Objects, [](const FLevelObject& Object) { return Object.Behaviour == "Player"; });
    // LoopLevels puts the carried player object where the new level's player starts.
    if (Carried && PlayerObject != Level->Objects.end())
    {
        const float X = PlayerObject->X;
        const float Y = PlayerObject->Y;
        *PlayerObject = Carried->Object;
        PlayerObject->X = X;
        PlayerObject->Y = Y;
    }

    // Level view: MAP_ layers drawn bottom-up with the placed objects. The player drives with the original movement code; levels
    // without a player fall back to scrolling with the arrow keys.
    const FSpriteSheet Blocks = Renderer.CreateSpriteSheet(Level->Blocks);
    std::expected<FObjectSprites, std::string> Objects = CreateObjectSprites(Archive, Renderer, *Level);
    if (!Objects)
    {
        return std::unexpected(Objects.error());
    }

    constexpr double ScrollSpeed = 600.0;
    const float MapWidth = float(Level->GridWidth * BlockWidth);
    const float MapHeight = float(Level->GridHeight * BlockHeight);
    std::optional<FMovementClass> PlayerClass;
    std::optional<FPlayerState> Player;
    std::optional<FTerrainCollision> Terrain;
    const GLFWvidmode* const DisplayMode = glfwGetVideoMode(glfwGetPrimaryMonitor());
    const uint32_t RefreshMillihertz = DisplayMode && DisplayMode->refreshRate > 0 ? uint32_t(DisplayMode->refreshRate) * MillihertzPerHertz : 0;
    const uint32_t FramePeriod = FramePeriodMilliseconds(RefreshMillihertz);
    std::optional<FCombat> Combat;
    // Session +0x101c: the last step, which the draw pass plays again.
    uint32_t LastStepMilliseconds = 0;
    // The smaller side of the player's current frame, which AnimUpdateBounds makes the body's bounds.
    float PlayerSpriteSize = 0.f;
    std::vector<FObjectBody> Bodies = CreateObjectBodies(*Level);
    const size_t PlayerIndex = size_t(PlayerObject - Level->Objects.begin());
    std::vector<FPickup> Pickups = CreatePickups(*Level);
    std::vector<std::optional<FActorAnimation>> ActorAnimations = CreateActorAnimations(Bodies, *Level, *Objects);
    std::expected<std::vector<FDoor>, std::string> Doors = CreateDoors(*Level, Archive, Bodies);
    if (!Doors)
    {
        std::fprintf(stderr, "Gates and lifts stay still: %s\n", Doors.error().c_str());
        Doors.emplace();
    }

    std::optional<std::string> NextLevel;
    // The panel's sheets, and one single-pixel frame per radar dot colour. ponytail: the dot colours follow the radar legend (red enemy,
    // yellow key or code, blue lift, green for the player) until the palette FUN_00467a10 indexes is traced.
    const FSpriteSheet PanelSheet = Panel ? Renderer.CreateSpriteSheet(Panel->GetBitmaps(EHudSheet::Panel)) : FSpriteSheet{};
    const FSpriteSheet VPanelSheet = Panel ? Renderer.CreateSpriteSheet(Panel->GetBitmaps(EHudSheet::VPanel)) : FSpriteSheet{};
    const FSpriteSheet BonusSheet = Panel ? Renderer.CreateSpriteSheet(Panel->GetBitmaps(EHudSheet::Bonus)) : FSpriteSheet{};
    FMessages Messages;
    constexpr std::array<uint16_t, 4> RadarColors = { 0x7c00, 0x03e0, 0x7fe0, 0x001f };
    std::vector<Gaf::FFrame> RadarPixels;
    for (const uint16_t Color : RadarColors)
    {
        RadarPixels.push_back({ .Width = 1, .Height = 1, .ColorKey = uint16_t(~Color & 0x7fff), .Pixels = { Color } });
    }

    const FSpriteSheet ColorSheet = Renderer.CreateSpriteSheet(RadarPixels);
    FPlayerInventory Inventory = Carried ? Carried->Inventory : PlayerObject != Level->Objects.end() ? CreateInventory(*PlayerObject) : FPlayerInventory{};
    if (PlayerObject != Level->Objects.end())
    {
        std::expected<FMovementClass, std::string> Class = ReadMovementClass(*PlayerObject);
        std::expected<FTerrainCollision, std::string> Collision = BuildTerrainCollision(*Level);
        if (Class && Collision)
        {
            PlayerClass = *Class;
            if (Start && !Carried)
            {
                PlayerObject->X = Start->first;
                PlayerObject->Y = Start->second;
            }

            Player = SpawnPlayer(*PlayerObject, *Class);
            // The carried player keeps its pose, speed and turn state; only its velocity is cleared.
            if (Carried)
            {
                const FPlayerState Arrival = *Player;
                *Player = Carried->State;
                Player->Position = Player->PreviousPosition = Arrival.Position;
                Player->Center = Arrival.Center;
                Player->Velocity = {};
            }

            Terrain = std::move(*Collision);
            if (std::expected<FCombat, std::string> Weapons = CreateCombat(*Level, *PlayerObject, Archive, Bodies))
            {
                Combat = std::move(*Weapons);
                // The carried player object brings its weapons, loaded first and in the same order, and the game clock runs on.
                if (Carried)
                {
                    Combat->Clock = Carried->Clock;
                    if (Carried->Weapon.Weapon < Combat->Weapons.size())
                    {
                        Combat->PlayerWeapon = Carried->Weapon;
                    }
                }

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

    const auto PanelValues = [&]
    {
        FPanelValues Values{
            .Shield = Inventory.Shield,
            .ShieldMax = Inventory.ShieldMax,
            .Energy = Inventory.Energy,
            .EnergyMax = Inventory.EnergyMax,
            .Money = Inventory.Money,
            .Kills = Inventory.Kills,
            .Enemies = CountEnemies(*Level, Bodies),
        };
        if (Player)
        {
            Values.Speed = Player->SpeedMagnitude;
            Values.Position = Player->Position;
        }

        if (Combat)
        {
            Values.TimeMilliseconds = Combat->Clock;
            // The weapon in hand's ammo stock (object +0x248).
            if (Combat->PlayerWeapon.Weapon < Combat->Weapons.size())
            {
                const std::string& AmmoName = Combat->Weapons[Combat->PlayerWeapon.Weapon].Ammo;
                const auto Stock = std::ranges::find_if(Inventory.Ammo, [&](const FAmmoStock& Ammo)
                {
                    return std::ranges::equal(Ammo.Name, AmmoName, [](const char A, const char B) { return std::tolower(uint8_t(A)) == std::tolower(uint8_t(B)); });
                });
                Values.Ammo = Stock == Inventory.Ammo.end() ? 0.f : Stock->Count;
            }
        }

        return Values;
    };

    FFrameClock Clock(FramePeriod);
    Clock.Poll(ClockMilliseconds());
    // ponytail: frame rate in the window title until the HUD can draw text.
    uint32_t FpsWindowStart = ClockMilliseconds();
    uint32_t FpsFrames = 0;
    // Development aid: N starts the next level, wrapping to the first after the last, always without a carried player.
    bool bNextKeyDown = glfwGetKey(Window, GLFW_KEY_N) == GLFW_PRESS;
    const auto Current = std::ranges::find(DebugLevels, LevelPath);
    const size_t CurrentIndex = Current == DebugLevels.end() ? DebugLevels.size() : size_t(Current - DebugLevels.begin());
    while (!glfwWindowShouldClose(Window))
    {
        glfwPollEvents();
        // ponytail: Esc quits straight away until the original pause menu is ported.
        if (glfwGetKey(Window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        {
            glfwSetWindowShouldClose(Window, GLFW_TRUE);
        }

        const bool bNextPressed = glfwGetKey(Window, GLFW_KEY_N) == GLFW_PRESS && !bNextKeyDown;
        bNextKeyDown = glfwGetKey(Window, GLFW_KEY_N) == GLFW_PRESS;
        if (bNextPressed && !DebugLevels.empty())
        {
            const size_t Next = (CurrentIndex + 1) % DebugLevels.size();
            std::printf("Debug: level %zu of %zu\n", Next + 1, DebugLevels.size());
            Carried.reset();
            return std::optional(DebugLevels[Next]);
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
                if (const FObjectBody* const Struck = StepPlayer(*Player, *PlayerClass, Command, StepMilliseconds, *Terrain, *Level, Bodies))
                {
                    ReportDoorContact(*Doors, *Struck, Inventory, Messages);
                }

                StepObjectBodies(Bodies, StepMilliseconds, *Terrain, *Level);
                StepActors(Combat ? &*Combat : nullptr, Bodies, StepMilliseconds, *Terrain, *Level, &*Player);
                const FPlayerTarget Rider{ .State = *Player, .Radius = PlayerClass->Radius, .Inventory = Inventory, .ObjectIndex = PlayerIndex };
                if (const FDoor* const Lift = StepDoors(*Doors, *Level, Bodies, Rider, StepMilliseconds, Messages); Lift && !NextLevel)
                {
                    // ponytail: the end of a tower (a starred 3do-file) has no sequence yet; the player stays.
                    if (Lift->NextLevel.starts_with('*'))
                    {
                        std::printf("Reached %s\n", Lift->NextLevel.c_str());
                        std::fflush(stdout);
                    }
                    else
                    {
                        NextLevel = NextLevelPath(LevelPath, *Lift);
                    }
                }
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
                        { .State = *Player, .Radius = PlayerClass->Radius, .Inventory = Inventory, .ObjectIndex = PlayerIndex }, Messages);
                    if (Inventory.Money != MoneyBefore || Inventory.Energy != EnergyBefore || Inventory.Shield != ShieldBefore)
                    {
                        // ponytail: no HUD or death sequence yet; the totals go to the console.
                        std::printf("Energy %.0f, shield %.0f, money %.0f\n", Inventory.Energy, Inventory.Shield, Inventory.Money);
                        std::fflush(stdout);
                    }
                }
                // Objects the step created, such as dropped items, join the pickups and the drawn objects.
                if (Combat)
                {
                    for (const size_t Created : Combat->CreatedObjects)
                    {
                        if (std::optional<FPickup> Pickup = CreatePickup(*Level, Created))
                        {
                            Pickups.push_back(std::move(*Pickup));
                        }

                        if (std::expected<void, std::string> Added = AddObjectSprites(*Objects, Archive, Renderer, *Level, Created); !Added)
                        {
                            std::fprintf(stderr, "%s\n", Added.error().c_str());
                        }
                    }

                    Combat->CreatedObjects.clear();
                    ActorAnimations.resize(Level->Objects.size());
                }

                if (CollectPickups(*Player, *PlayerClass, Inventory, Pickups, *Level, Messages) != 0)
                {
                    std::printf("Energy %.0f, shield %.0f, money %.0f\n", Inventory.Energy, Inventory.Shield, Inventory.Money);
                    std::fflush(stdout);
                }

                // The update pass of MessagesUpdate (004315f0) follows the game update.
                Messages.Update(int32_t(StepMilliseconds), 0, 0, nullptr);

                // The update pass after each step uses the view the previous frame scrolled to.
                const FViewCells View = ViewCellsAround(int(std::floor(CameraX)), int(std::floor(CameraY)), *Level);
                if (Combat)
                {
                    StepObjectBobs(*Combat, *Level, Bodies, StepMilliseconds, View);
                }

                AnimateActors(ActorAnimations, Bodies, *Level, *Objects, View, int32_t(StepMilliseconds));
                // The panel's update pass follows the objects' (PanelUpdate at 00435000 with its draw flag clear).
                if (Panel)
                {
                    Panel->Step(PanelValues(), int32_t(StepMilliseconds));
                }
                LastStepMilliseconds = StepMilliseconds;
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
            for (const FObjectBody& Body : Bodies)
            {
                FLevelObject& Object = Level->Objects[Body.ObjectIndex];
                Object.X = Body.State.Position.X;
                Object.Y = Body.State.Position.Y;
                // A turret turns its top; its base sprite stays fixed.
                if (Body.Turret)
                {
                    Object.Direction = Body.Turret->Direction;
                }
                else if (Body.Actor)
                {
                    Object.Direction = Body.State.Direction;
                    Object.TopPose = Body.Actor->bTwoPieces ? Body.Actor->TopPose : 0;
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

        CameraX = std::clamp(CameraX, 0.0, std::max(0.0, double(MapWidth - ScreenWidth)));
        CameraY = std::clamp(CameraY, 0.0, std::max(0.0, double(MapHeight - ScreenHeight)));
        AnimateActors(ActorAnimations, Bodies, *Level, *Objects, ViewCellsAround(int(std::floor(CameraX)), int(std::floor(CameraY)), *Level),
            int32_t(LastStepMilliseconds));
        UpdateObjectSprites(*Objects, *Level, ActorAnimations, *Doors);
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

        const auto DrawHud = [&](const std::span<const FHudSprite> HudSprites)
        {
            for (const FHudSprite& Hud : HudSprites)
            {
                const FSpriteSheet& Sheet = Hud.Sheet == EHudSheet::Panel    ? PanelSheet
                                            : Hud.Sheet == EHudSheet::VPanel ? VPanelSheet
                                            : Hud.Sheet == EHudSheet::Bonus  ? BonusSheet
                                                                             : ColorSheet;
                const Gaf::FAnimationFrame& Frame = Hud.Frame;
                if (Frame.BitmapIndex != 0)
                {
                    Renderer.DrawSprite(Sheet, Frame.BitmapIndex - 1, Hud.X + float(Frame.OffsetX), Hud.Y + float(Frame.OffsetY), Frame.bMirrorX, Frame.bMirrorY,
                        FrameBlend(Frame), 1.f, 1.f, Hud.VisibleColumns, Hud.Diffuse, Hud.Specular);
                }
            }
        };

        // The messages' draw pass comes after the objects and before the panel, with the last step.
        if (Panel)
        {
            std::vector<FHudSprite> MessageSprites;
            const FMessages::FDraw DrawMessage = [&](const FMessage& Message, const int32_t X, const int32_t Y, const std::string_view Text)
            {
                Panel->DrawMessage(MessageSprites, Message, X, Y, Text);
            };
            Messages.Update(int32_t(LastStepMilliseconds), OriginX, OriginY, &DrawMessage);
            DrawHud(MessageSprites);
        }

        if (Panel && Player)
        {
            const std::vector<FRadarDot> Radar = FindRadarDots(*Level, Bodies, Player->Position, PlayerIndex, int(ScreenHeight));
            DrawHud(Panel->Draw(PanelValues(), Radar, int(ScreenWidth), int(ScreenHeight)));
            FPanel::PostWarnings(PanelValues(), Messages);
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

        // The level ends after the frame in which the player took a lift.
        if (NextLevel && Player && PlayerObject != Level->Objects.end())
        {
            Carried = FCarriedPlayer{
                .Object = *PlayerObject,
                .State = *Player,
                .Inventory = Inventory,
                .Weapon = Combat ? Combat->PlayerWeapon : FWeaponState{},
                .Clock = Combat ? Combat->Clock : 1,
            };
            return NextLevel;
        }
    }

    return std::optional<std::string>();
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
    // Development aid: --dump-gaf PATH lists an archive GAF's animations with their frame counts and durations.
    std::optional<std::string> DumpGafPath;
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
        else if (std::strcmp(Args[Index], "--dump-gaf") == 0 && Index + 1 < ArgCount)
        {
            DumpGafPath = Args[++Index];
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
    if (DumpGafPath)
    {
        const std::optional<FObjectGaf> Gaf = LoadObjectGaf(Archive, *DumpGafPath);
        if (!Gaf)
        {
            std::fprintf(stderr, "Cannot decode %s\n", DumpGafPath->c_str());
            return 1;
        }

        for (size_t Index = 0; Index < Gaf->Animations.size(); ++Index)
        {
            const Gaf::FAnimation& Animation = Gaf->Animations[Index];
            std::printf("%3zu %-12s %3zu frames, flags %#x, loop %u, durations", Index + 1, Animation.Name.c_str(), Animation.Frames.size(), Animation.Flags,
                Animation.LoopStart);
            for (const Gaf::FAnimationFrame& Frame : Animation.Frames)
            {
                std::printf(" %d", Frame.Duration);
            }
            std::printf("\n");
        }

        const std::optional<std::vector<uint8_t>> File = Archive.Read(*DumpGafPath);
        const std::optional<std::vector<Font::FFont>> Fonts = File ? Font::LoadFonts(*File) : std::nullopt;
        for (size_t Index = 0; Fonts && Index < Fonts->size(); ++Index)
        {
            const Font::FFont& Font = (*Fonts)[Index];
            std::printf("font %zu: flags %#x, advance %d, line %d, mode %u, alpha %#x, diffuse %#08x, specular %#08x\n", Index + 1, Font.Flags, Font.Advance,
                Font.LineHeight, Font.DrawMode, Font.Color.Alpha, Font.Color.Diffuse, Font.Color.Specular);
        }
        return 0;
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
        const std::expected<std::string, std::string> Doors = VerifyDoors(Archive);
        std::printf("Gates and lifts: %s\n", Doors ? Doors->c_str() : Doors.error().c_str());
        std::printf("Frame loop: %s, terrain collision: %s, player movement: %s\n", bFrameLoop ? "passed" : "failed",
            Collision ? "passed" : Collision.error().c_str(), Movement ? "passed" : Movement.error().c_str());
        return bFrameLoop && Collision && Movement && Paths && Demo && Combat && Patrols && Doors ? 0 : 1;
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
    std::expected<FPanel, std::string> Panel = FPanel::Load(Archive);
    if (!Panel)
    {
        std::fprintf(stderr, "No panel: %s\n", Panel.error().c_str());
    }
    std::optional<FCarriedPlayer> Carried;
    // Every level in the archive by file name, so each tower's floors follow each other, for the debug next-level key.
    std::vector<std::string> DebugLevels;
    for (const std::string& Name : Archive.GetNames())
    {
        if (Name.ends_with(".3do"))
        {
            DebugLevels.push_back(Name.substr(0, Name.size() - 4));
        }
    }

    const auto FileName = [](const std::string& Path) { return std::string_view(Path).substr(Path.rfind('\\') + 1); };
    std::ranges::sort(DebugLevels, [&](const std::string& A, const std::string& B) { return std::pair(FileName(A), A) < std::pair(FileName(B), B); });
    for (size_t Index = 0; Index < DebugLevels.size(); ++Index)
    {
        std::printf("Debug level %zu: %s\n", Index + 1, DebugLevels[Index].c_str());
    }

    int Result = 0;
    for (std::string Path = LevelPath;;)
    {
        const std::expected<std::optional<std::string>, std::string> Next = RunLevel(Window, Renderer, Archive, Path, Start, bAutoFire, Carried, DebugLevels, Panel ? &*Panel : nullptr);
        Renderer.ReleaseSpriteSheets();
        // --start places the player in the first level only.
        Start.reset();
        if (!Next)
        {
            std::fprintf(stderr, "%s\n", Next.error().c_str());
            Result = 1;
            break;
        }

        if (!*Next)
        {
            break;
        }

        Path = **Next;
    }

    Renderer.Shutdown();
    glfwDestroyWindow(Window);
    glfwTerminate();
    return Result;
}
