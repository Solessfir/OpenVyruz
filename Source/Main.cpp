#include "Game/Audio.h"
#include "Game/Collision.h"
#include "Game/Engine.h"
#include "Game/Level.h"
#include "Game/Menu.h"
#include "Game/Score.h"
#include "Game/Session.h"
#include "Game/Loop.h"
#include "Game/Panel.h"
#include "Pg/Avi.h"
#include "Pg/File.h"
#include "Pg/Font.h"
#include "Pg/Tga.h"
#include "Pg/Gaf.h"
#include "Pg/Indeo5.h"
#include "Pg/ResourceArchive.h"
#include "Pg/Sound.h"
#include "Render/Renderer.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
// The game's sound, which the menus and the level loop reach as the original reaches its globals; null while it has no output.
FAudio* GameAudio = nullptr;

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
    int Images = 0;
    int Sounds = 0;
    size_t SoundEvents = 0;
    size_t SilentEvents = 0;
    for (const std::string& Name : Archive.GetNames())
    {
        if (Name.ends_with(".wav"))
        {
            const std::optional<std::vector<uint8_t>> File = Archive.Read(Name);
            if (!File || !Sound::DecodeWav(*File))
            {
                std::printf("FAIL %s\n", Name.c_str());
                ++Failed;
                continue;
            }

            ++Sounds;
            continue;
        }

        // The story and ending screens' Targa images decode as well.
        if (Name.ends_with(".tga"))
        {
            const std::optional<std::vector<uint8_t>> File = Archive.Read(Name);
            if (!File || !LoadTga(*File))
            {
                std::printf("FAIL %s\n", Name.c_str());
                ++Failed;
                continue;
            }

            ++Images;
            continue;
        }

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

        // A frame with a sound event (flag 1) plays its GAF's SMP_ sample by event value; AnimationFrameSound (0045ea70) skips a zero value or a missing sample, which panel.gaf, v-panel.gaf and circular-fx.gaf have.
        constexpr uint32_t SoundEventFlag = 1;
        for (const Gaf::FAnimation& Animation : Gaf->Animations)
        {
            for (const Gaf::FAnimationFrame& Frame : Animation.Frames)
            {
                const bool bPlays = Animation.Samples && Frame.EventValue >= 1 && Frame.EventValue <= Animation.Samples->size();
                SoundEvents += (Frame.EventFlags & SoundEventFlag) != 0 && bPlays ? 1 : 0;
                SilentEvents += (Frame.EventFlags & SoundEventFlag) != 0 && !bPlays ? 1 : 0;
            }
        }

        ++Decoded;
        FrameCount += Gaf->Bitmaps.size();
    }
    std::printf("GAF: %d decoded (%zu frames, %zu sound events, %zu silent), %d TGA images and %d WAV sounds, %d failed\n", Decoded, FrameCount, SoundEvents, SilentEvents, Images,
        Sounds, Failed);
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
    // A Use-delta-packing lift or gate stores later frames as changes over the first, which ObjectDraw (00431ae0, FUN_00433710) draws underneath first.
    bool bDeltaBase = false;
    // A turret's fixed base: the animation after its 24 top poses, drawn under the top (ObjectDraw, kind 0x1e).
    bool bTurretBase = false;
    Gaf::FAnimationFrame Frame;
    float X = 0.f;
    float Y = 0.f;
    int64_t Depth = 0;
    ESpriteBlend Blend = ESpriteBlend::Opaque;
    float Stretch = 1.f;
    uint32_t Diffuse = 0xffffff;
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

std::optional<FActorAnimation> CreateActorAnimation(const FObjectBody& Body, const FLevel& Level, const FObjectSprites& Sprites)
{
    const FLevelObject& Object = Level.Objects[Body.ObjectIndex];
    const std::vector<Gaf::FAnimation>* const Animations = FindAnimations(Sprites, Object.GafPath);
    if (!Body.Actor || !Animations || Object.GafIndex < 1)
    {
        return std::nullopt;
    }

    FActorAnimation Actor{ .PreviousPose = Body.State.Pose };
    const size_t WalkCount = Object.SubType == "walker" ? size_t(ObjectDirectionCount) : Object.SubType == "spider" ? size_t(FourWayDirectionCount) : 0;
    for (size_t Walk = 0; Walk < WalkCount && size_t(Object.GafIndex - 1) + Walk < Animations->size(); ++Walk)
    {
        Actor.Walks.push_back(StartAnimation((*Animations)[size_t(Object.GafIndex - 1) + Walk]));
    }

    return Actor;
}

std::vector<std::optional<FActorAnimation>> CreateActorAnimations(const std::span<const FObjectBody> Bodies, const FLevel& Level, const FObjectSprites& Sprites)
{
    std::vector<std::optional<FActorAnimation>> Result(Level.Objects.size());
    for (const FObjectBody& Body : Bodies)
    {
        Result[Body.ObjectIndex] = CreateActorAnimation(Body, Level, Sprites);
    }

    return Result;
}

// The looping animation a placed object other than an actor, a door or a turret plays: Water-Wave FX, items, things.
// Graphic-update-needed classes (the FX, thing, ammo and decor libraries) advance in EngineUpdateObjects (004406e0) every step wherever they are; the others in ObjectDraw (00431ae0) while in the view's cells, in the update pass and again in the draw pass.
struct FPlacedAnimation
{
    size_t ObjectIndex = 0;
    size_t SheetIndex = 0;
    size_t Animation = 0;
    bool bGraphicUpdate = false;
    FAnimationPlayback Playback;
};

std::vector<FPlacedAnimation> CreatePlacedAnimations(const FObjectSprites& Sprites, const FLevel& Level, const std::span<const std::optional<FActorAnimation>> Actors)
{
    constexpr uint32_t LoopFlag = 2;
    std::vector<FPlacedAnimation> Result;
    for (const FObjectSprite& Sprite : Sprites.Sprites)
    {
        const FLevelObject& Object = Level.Objects[Sprite.ObjectIndex];
        if (Sprite.bShadow || Sprite.bTop || Sprite.bTurretBase || Sprite.bDeltaBase || Actors[Sprite.ObjectIndex] || Object.SubType == "lift" || Object.SubType == "gate" ||
            Object.SubType == TurretKind || Object.SubType == "vyruz")
        {
            continue;
        }

        const size_t Index = ObjectAnimationIndex(Object, false);
        const Gaf::FAnimation& Animation = Sprites.Animations[Sprite.SheetIndex][Index];
        if (Animation.Frames.size() > 1 && (Animation.Flags & LoopFlag) != 0)
        {
            Result.push_back({
                .ObjectIndex = Sprite.ObjectIndex,
                .SheetIndex = Sprite.SheetIndex,
                .Animation = Index,
                .bGraphicUpdate = ParseLevelFloat(Object.Properties.Find("Graphic-update-needed")).value_or(0.f) != 0.f,
                .Playback = StartAnimation(Animation),
            });
        }
    }

    return Result;
}

void StepPlacedAnimations(std::vector<FPlacedAnimation>& Animations, const FObjectSprites& Sprites, const FLevel& Level, const FViewCells View, const int32_t StepMilliseconds,
    const bool bDrawPass)
{
    for (FPlacedAnimation& Placed : Animations)
    {
        const FLevelObject& Object = Level.Objects[Placed.ObjectIndex];
        const bool bSteps = Placed.bGraphicUpdate ? !bDrawPass : View.Contains({ .X = Object.X, .Y = Object.Y });
        if (bSteps && !Object.bRemoved)
        {
            const FSoundEmitter Emitter = MakeSoundEmitter(Object.Properties.Find("Sample-channel-range"), Object.Properties.Find("Sample-channel-priority"), Object.X, Object.Y,
                Object.Altitude);
            UpdateAnimation(Placed.Playback, Sprites.Animations[Placed.SheetIndex][Placed.Animation], StepMilliseconds, &Emitter);
        }
    }
}

// The actor kinds of ObjectDraw (00431ae0), which runs for every object in the view's cells once per step in the update pass and again in the frame's draw pass; both advance walks by the step.
// Cops, jeeps, tanks and helis show their pose as a frame of a fixed animation; jeep and tank animations have a single looping frame, so playing them changes nothing.
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
        // ObjectDraw draws a burner (kind 0x1a) like a cop: its lean animations exist only for the player, so it always shows the rot animation.
        if (Object.SubType == "cop" || Object.SubType == "burner")
        {
            Actor->BodyAnimation = CopRotAnimation;
        }
        else if (Object.SubType == "thing")
        {
            // A moving thing keeps its one animation (ObjectDraw's thing case picks one per direction, and the Particle-of-Death has a single one).
            Actor->BodyAnimation = Base;
            Actor->BodyFrame = 1;
        }
        else if (Object.SubType == "jeep" || Object.SubType == "roller" || Object.SubType == TankKind)
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

// EngineVisitViewObjects (00433b30) steps the map animations in the view's cells in its update pass, each once the clock has passed its start time, playing their sounds from the cell on any channel at priority 0x3f (AnimStampSoundChannels from ObjectInitialize).
// Low details (Video Details) leave them out.
void StepDecorAnimations(std::vector<FDecorAnimation>& DecorAnimations, const FObjectSprites& Sprites, const FViewCells View, const uint32_t Clock, const int32_t StepMilliseconds)
{
    constexpr uint32_t DecorSoundPriority = 0x3f;
    for (FDecorAnimation& Decor : DecorAnimations)
    {
        const auto Sheet = Sprites.SheetIndices.find(Decor.GafPath);
        if (Decor.StartMilliseconds >= Clock || Sheet == Sprites.SheetIndices.end() || !View.Contains({ .X = Decor.X, .Y = Decor.Y }))
        {
            continue;
        }

        const FSoundEmitter Emitter{ .X = Decor.X, .Y = Decor.Y, .ChannelMask = Sound::FMixer::AllChannels, .Priority = DecorSoundPriority };
        UpdateAnimation(Decor.Playback, Sprites.Animations[Sheet->second][Decor.Animation], StepMilliseconds, &Emitter);
    }
}

// The draw pass of EngineVisitViewObjects draws each map animation's frame at its cell in the middle sprite layer, its depth biased by its terrain type.
void AddDecorSprites(std::vector<FObjectSprite>& Out, const std::span<const FDecorAnimation> DecorAnimations, const FObjectSprites& Sprites, const FViewCells View)
{
    for (const FDecorAnimation& Decor : DecorAnimations)
    {
        const auto Sheet = Sprites.SheetIndices.find(Decor.GafPath);
        if (Sheet == Sprites.SheetIndices.end() || !View.Contains({ .X = Decor.X, .Y = Decor.Y }))
        {
            continue;
        }

        const Gaf::FAnimation& Animation = Sprites.Animations[Sheet->second][Decor.Animation];
        if (Decor.Playback.bEnded || Decor.Playback.Frame == 0 || Decor.Playback.Frame > Animation.Frames.size())
        {
            continue;
        }

        const Gaf::FAnimationFrame& Frame = Animation.Frames[Decor.Playback.Frame - 1];
        Out.push_back({
            .SheetIndex = Sheet->second,
            .ObjectIndex = NoObjectIndex,
            .Frame = Frame,
            .X = Decor.X,
            .Y = Decor.Y,
            .Depth = (int64_t(Decor.Y) + Decor.DepthBias) * SpriteDepthLayerCount + MiddleDepthLayer,
            .Blend = FrameBlend(Frame),
        });
    }
}

void DrawHudSprite(FRenderer& Renderer, const FSpriteSheet& Sheet, const FHudSprite& Hud)
{
    const Gaf::FAnimationFrame& Frame = Hud.Frame;
    if (Frame.BitmapIndex != 0)
    {
        // A stretched frame's hotspot offset is scaled and truncated like the original's __ftol.
        const float OffsetX = Hud.ScaleX != 1.f ? std::trunc(float(Frame.OffsetX) * Hud.ScaleX) : float(Frame.OffsetX);
        const float OffsetY = Hud.ScaleY != 1.f ? std::trunc(float(Frame.OffsetY) * Hud.ScaleY) : float(Frame.OffsetY);
        Renderer.DrawSprite(Sheet, Frame.BitmapIndex - 1, Hud.X + OffsetX, Hud.Y + OffsetY, Frame.bMirrorX, Frame.bMirrorY,
            Hud.Alpha < 1.f ? ESpriteBlend::Alpha : FrameBlend(Frame), Hud.ScaleX, Hud.ScaleY, Hud.VisibleColumns, Hud.Diffuse, Hud.Specular, Hud.Alpha);
    }
}

// The map's picture (MapToggle at 00436460 with GobRenderMap at 0046b120): every MAP_ layer from the first, each block stretched into its cell's share of a screen-sized bitmap, which starts black; pixels left at 0x800 are transparent. ponytail: point-samples the level instead of stretching block by block, so cell edges may round a pixel differently.
Gaf::FFrame RenderLevelMap(const FLevel& Level)
{
    constexpr uint16_t MapColorKey = 0x800;
    const uint32_t Width = uint32_t(ScreenWidth);
    const uint32_t Height = uint32_t(ScreenHeight);
    Gaf::FFrame Map{ .Width = Width, .Height = Height, .ColorKey = MapColorKey, .Pixels = std::vector<uint16_t>(size_t(Width) * Height, 0) };
    const uint64_t MapWidth = uint64_t(Level.GridWidth) * BlockWidth;
    const uint64_t MapHeight = uint64_t(Level.GridHeight) * BlockHeight;
    if (MapWidth == 0 || MapHeight == 0)
    {
        return Map;
    }

    for (uint32_t Y = 0; Y < Height; ++Y)
    {
        const uint64_t WorldY = Y * MapHeight / Height;
        for (uint32_t X = 0; X < Width; ++X)
        {
            const uint64_t WorldX = X * MapWidth / Width;
            const size_t Cell = size_t(WorldY / BlockHeight) * Level.GridWidth + size_t(WorldX / BlockWidth);
            for (uint32_t Layer = 0; Layer < Level.LayerCount; ++Layer)
            {
                const uint32_t Block = Level.Map[size_t(Layer) * Level.GridWidth * Level.GridHeight + Cell];
                if (Block == 0 || Block > Level.Blocks.size())
                {
                    continue;
                }

                const Gaf::FFrame& Bitmap = Level.Blocks[Block - 1];
                const int64_t PixelX = int64_t(WorldX % BlockWidth) - Bitmap.TrimX;
                const int64_t PixelY = int64_t(WorldY % BlockHeight) - Bitmap.TrimY;
                if (PixelX < 0 || PixelY < 0 || PixelX >= int64_t(Bitmap.Width) || PixelY >= int64_t(Bitmap.Height))
                {
                    continue;
                }

                const uint16_t Pixel = Bitmap.Pixels[size_t(PixelY) * Bitmap.Width + size_t(PixelX)];
                if (Pixel != Bitmap.ColorKey)
                {
                    Map.Pixels[size_t(Y) * Width + X] = Pixel;
                }
            }
        }
    }

    return Map;
}

// ColorCombine at 0044f310 scales each diffuse channel by the other colour's through ColorModulate (0044e040), rounding down like it.
constexpr uint32_t ModulateColor(const uint32_t Color, const uint32_t By)
{
    constexpr uint32_t ChannelMask = 0xff;
    constexpr uint32_t ChannelBits = 8;
    uint32_t Result = 0;
    for (const uint32_t Shift : { 0u, ChannelBits, 2 * ChannelBits })
    {
        const uint32_t Channel = (Color >> Shift) & ChannelMask;
        const uint32_t Removed = ChannelMask - ((By >> Shift) & ChannelMask);
        Result |= (Channel - (((Removed + 1) * Channel) >> ChannelBits)) << Shift;
    }

    return Result;
}
static_assert(ModulateColor(0xd5d5d5, 0xffffff) == 0xd5d5d5);
static_assert(ModulateColor(0xffffff, 0x808080) == 0x808080);

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

void UpdateObjectSprites(FObjectSprites& Sprites, const FLevel& Level, const std::vector<std::optional<FActorAnimation>>& Actors, const std::span<const FDoor> Doors,
    const std::span<const FPlacedAnimation> PlacedAnimations)
{
    std::vector<const FDoor*> DoorOf(Level.Objects.size(), nullptr);
    for (const FDoor& Door : Doors)
    {
        DoorOf[Door.ObjectIndex] = &Door;
    }

    std::vector<const FPlacedAnimation*> PlacedOf(Level.Objects.size(), nullptr);
    for (const FPlacedAnimation& Placed : PlacedAnimations)
    {
        PlacedOf[Placed.ObjectIndex] = &Placed;
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

        if (const FPlacedAnimation* const Placed = PlacedOf[Sprite.ObjectIndex]; Placed && !Sprite.bShadow)
        {
            const std::vector<Gaf::FAnimationFrame>& Frames = Animations[Placed->Animation].Frames;
            const uint32_t Frame = Placed->Playback.bEnded ? 0 : Placed->Playback.Frame;
            Sprite.Frame = Frame >= 1 && Frame <= Frames.size() ? Frames[Frame - 1] : Gaf::FAnimationFrame{};
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

// The original took the mouse for itself through DirectInput, so the system cursor never showed and never left the game; it draws its own where it wants one.
// The port keeps the cursor inside the window (GLFW lets go while the window is unfocused) and hides it with a blank image.
GLFWcursor* BlankCursor = nullptr;
// Windows can drop the confinement on some focus changes, so it is applied again whenever the game becomes active.
bool bCursorToCapture = true;

void CaptureCursor(GLFWwindow* const Window)
{
    if (!BlankCursor)
    {
        static std::array<unsigned char, 4> Pixel{};
        const GLFWimage Image{ .width = 1, .height = 1, .pixels = Pixel.data() };
        BlankCursor = glfwCreateCursor(&Image, 0, 0);
    }

    glfwSetInputMode(Window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    glfwSetInputMode(Window, GLFW_CURSOR, GLFW_CURSOR_CAPTURED);
    glfwSetCursor(Window, BlankCursor);
    bCursorToCapture = false;
}

uint32_t ClockMilliseconds()
{
    return uint32_t(ClockNowMilliseconds());
}

// PgPoll's message pump (004813c0): once the window procedure has found the game inactive (AppActivate at 004814c0) and paused the clock (PgClockPause at 00469730), it blocks in GetMessage until the game is active again, so nothing moves meanwhile.
// Minimized or without the focus counts as inactive; on Wayland a window on a hidden workspace loses the focus, so nothing is drawn or presented for it.
void PollEvents(GLFWwindow* const Window)
{
    glfwPollEvents();
    const auto IsInactive = [Window]
    { return !glfwWindowShouldClose(Window) && (glfwGetWindowAttrib(Window, GLFW_ICONIFIED) != 0 || glfwGetWindowAttrib(Window, GLFW_FOCUSED) == 0); };
    if (!IsInactive())
    {
        if (bCursorToCapture)
        {
            CaptureCursor(Window);
        }

        return;
    }

    bCursorToCapture = true;

    PauseClock(true);
    if (GameAudio)
    {
        GameAudio->SetPaused(true);
    }

    while (IsInactive())
    {
        glfwWaitEvents();
    }

    if (GameAudio)
    {
        GameAudio->SetPaused(false);
    }

    PauseClock(false);
}

// ShowImageWait at 00404950: an image centred on the screen for up to 5 seconds; WaitForInput (00404910) ends the wait early on a key or a mouse button.
void ShowImage(GLFWwindow* const Window, FRenderer& Renderer, FResourceArchive& Archive, const std::string_view Path)
{
    constexpr uint32_t WaitMilliseconds = 5000;
    const std::optional<FObjectGaf> Gaf = LoadObjectGaf(Archive, Path);
    if (!Gaf || Gaf->Animations.empty() || Gaf->Animations.front().Frames.empty() || Gaf->Animations.front().Frames.front().BitmapIndex == 0)
    {
        return;
    }

    const Gaf::FAnimationFrame& Frame = Gaf->Animations.front().Frames.front();
    const Gaf::FFrame& Bitmap = Gaf->Bitmaps[Frame.BitmapIndex - 1];
    const FSpriteSheet Sheet = Renderer.CreateSpriteSheet(Gaf->Bitmaps);
    const float X = float((int(ScreenWidth) - int(Bitmap.Width)) / 2 - Bitmap.TrimX);
    const float Y = float((int(ScreenHeight) - int(Bitmap.Height)) / 2 - Bitmap.TrimY);
    const auto AnyInput = [Window]
    {
        for (int Key = GLFW_KEY_SPACE; Key <= GLFW_KEY_LAST; ++Key)
        {
            if (glfwGetKey(Window, Key) == GLFW_PRESS)
            {
                return true;
            }
        }

        return glfwGetMouseButton(Window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS || glfwGetMouseButton(Window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    };

    const uint32_t Start = ClockMilliseconds();
    while (!glfwWindowShouldClose(Window) && ClockMilliseconds() - Start < WaitMilliseconds)
    {
        PollEvents(Window);
        if (AnyInput())
        {
            break;
        }

        if (Renderer.BeginFrame())
        {
            Renderer.DrawSprite(Sheet, Frame.BitmapIndex - 1, X, Y);
            Renderer.EndFrame();
        }
    }
}

// The story and ending screens' text page (ShowTextPage at 004027f0 with the callback at 00402640): a full-screen Targa image and a text in v-bonus.gaf font 4 with word wrap from Left, 100 to the screen's right edge, until a key or mouse button goes down (FUN_00401880).
// A non-zero timeout also closes it, counted in display frames by FUN_00450d50.
void ShowTextPage(GLFWwindow* const Window, FRenderer& Renderer, FResourceArchive& Archive, const FPanel& Panel, const std::string_view ImagePath,
    const std::string_view TextPath, const int32_t Left, const uint32_t TimeoutMilliseconds = 0)
{
    constexpr size_t PageFont = 4;
    constexpr int32_t PageTop = 100;
    const std::optional<std::vector<uint8_t>> ImageFile = Archive.Read(ImagePath);
    const std::optional<std::vector<uint8_t>> TextFile = Archive.Read(TextPath);
    const std::optional<Gaf::FFrame> Image = ImageFile ? LoadTga(*ImageFile) : std::nullopt;
    if (!Image || !TextFile)
    {
        return;
    }

    const FSpriteSheet ImageSheet = Renderer.CreateSpriteSheet({ *Image });
    const FSpriteSheet FontSheet = Renderer.CreateSpriteSheet(Panel.GetBitmaps(EHudSheet::Bonus));
    const std::string Text(TextFile->begin(), TextFile->end());
    const std::vector<FHudSprite> Glyphs = Panel.DrawPage(PageFont, Text, Left, PageTop, int32_t(ScreenWidth));
    const auto Inputs = [Window]
    {
        std::vector<bool> Down;
        for (int Key = GLFW_KEY_SPACE; Key <= GLFW_KEY_LAST; ++Key)
        {
            Down.push_back(glfwGetKey(Window, Key) == GLFW_PRESS);
        }

        Down.push_back(glfwGetMouseButton(Window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS);
        Down.push_back(glfwGetMouseButton(Window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS);
        return Down;
    };

    std::vector<bool> Previous = Inputs();
    const uint32_t Start = ClockMilliseconds();
    while (!glfwWindowShouldClose(Window) && (TimeoutMilliseconds == 0 || ClockMilliseconds() - Start < TimeoutMilliseconds))
    {
        PollEvents(Window);
        const std::vector<bool> Now = Inputs();
        bool bPressed = false;
        for (size_t Index = 0; Index < Now.size(); ++Index)
        {
            bPressed = bPressed || (Now[Index] && !Previous[Index]);
        }

        if (bPressed)
        {
            break;
        }

        Previous = Now;
        if (Renderer.BeginFrame())
        {
            Renderer.DrawSprite(ImageSheet, 0, 0.f, 0.f);
            for (const FHudSprite& Glyph : Glyphs)
            {
                const Gaf::FAnimationFrame& Frame = Glyph.Frame;
                Renderer.DrawSprite(FontSheet, Frame.BitmapIndex - 1, Glyph.X + float(Frame.OffsetX), Glyph.Y + float(Frame.OffsetY), false, false, FrameBlend(Frame),
                    1.f, 1.f, UINT32_MAX, Glyph.Diffuse, Glyph.Specular);
            }

            Renderer.EndFrame();
        }
    }
}

// A key or mouse button going down, as WaitForInput (00404910) and FUN_00401880 wait for one.
class FInputPress
{
public:
    explicit FInputPress(GLFWwindow* const Window) : Window(Window), Previous(Read()) {}

    bool Poll()
    {
        const std::vector<bool> Now = Read();
        bool bPressed = false;
        for (size_t Index = 0; Index < Now.size(); ++Index)
        {
            bPressed = bPressed || (Now[Index] && !Previous[Index]);
        }

        Previous = Now;
        return bPressed;
    }

private:
    std::vector<bool> Read() const
    {
        std::vector<bool> Down;
        for (int Key = GLFW_KEY_SPACE; Key <= GLFW_KEY_LAST; ++Key)
        {
            Down.push_back(glfwGetKey(Window, Key) == GLFW_PRESS);
        }

        Down.push_back(glfwGetMouseButton(Window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS);
        Down.push_back(glfwGetMouseButton(Window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS);
        return Down;
    }

    GLFWwindow* Window;
    std::vector<bool> Previous;
};

// FUN_00446120 when the finale ends: over a black screen the lines of PTR_s_____GENERAL_NETWORK_FAILURE_____0050d740 type out at 100, from 100 and 20 apart, one a second, until 10 seconds pass or a key or button goes down.
void ShowNetworkFailure(GLFWwindow* const Window, FRenderer& Renderer, const FPanel& Panel)
{
    constexpr std::array<std::string_view, 5> Lines = { "... GENERAL NETWORK FAILURE ...", "", "... DISCONNECTED ...", "", "... SYSTEM HALTED ..." };
    constexpr int32_t LineX = 100;
    constexpr int32_t FirstLineY = 100;
    constexpr int32_t LineStep = 0x14;
    constexpr int32_t LinePeriodMilliseconds = 1000;
    constexpr int32_t ScreenMilliseconds = 10000;
    constexpr uint32_t LineLifeMilliseconds = 360000;
    // MessageAddText gets the panel's font (session +0x20a8), the port's panel font 3.
    constexpr uint32_t LineFont = 3;
    // FUN_004315b0 colours each line green.
    constexpr uint32_t LineColor = 0xff00;
    const FSpriteSheet Sheet = Renderer.CreateSpriteSheet(Panel.GetBitmaps(EHudSheet::Panel));
    FMessages Messages;
    FInputPress Input(Window);
    uint32_t Last = ClockMilliseconds();
    int32_t Elapsed = 0;
    int32_t LineTimer = 0;
    size_t Line = 0;
    while (!glfwWindowShouldClose(Window) && Elapsed < ScreenMilliseconds)
    {
        PollEvents(Window);
        if (Input.Poll())
        {
            break;
        }

        const uint32_t Now = ClockMilliseconds();
        const int32_t Step = int32_t(Now - Last);
        Last = Now;
        Elapsed += Step;
        LineTimer -= Step;
        if (LineTimer < 1 && Line < Lines.size())
        {
            LineTimer = LinePeriodMilliseconds;
            Messages.Add({
                .Text = std::string(Lines[Line]),
                .X = float(LineX),
                .Y = float(FirstLineY + LineStep * int32_t(Line)),
                .LifeMilliseconds = LineLifeMilliseconds,
                .bTypewriter = true,
                .Gaf = EMessageGaf::Panel,
                .Font = LineFont,
                .Color = { .Diffuse = LineColor },
            }, EMessagePlacement::Always);
            ++Line;
        }

        if (Renderer.BeginFrame())
        {
            std::vector<FHudSprite> Sprites;
            const FMessages::FDraw Draw = [&](const FMessage& Message, const int32_t X, const int32_t Y, const std::string_view Text)
            {
                Panel.DrawMessage(Sprites, Message, X, Y, Text);
            };
            Messages.Update(Step, 0, 0, &Draw);
            for (const FHudSprite& Hud : Sprites)
            {
                DrawHudSprite(Renderer, Sheet, Hud);
            }

            Renderer.EndFrame();
        }
    }
}

uint32_t DisplayFramePeriod()
{
    const GLFWvidmode* const DisplayMode = glfwGetVideoMode(glfwGetPrimaryMonitor());
    const uint32_t RefreshMillihertz = DisplayMode && DisplayMode->refreshRate > 0 ? uint32_t(DisplayMode->refreshRate) * MillihertzPerHertz : 0;
    return FramePeriodMilliseconds(RefreshMillihertz);
}

// The window's letterbox scale and the screen's top left in it.
struct FLetterbox
{
    double Scale = 1.0;
    double Left = 0.0;
    double Top = 0.0;
};

FLetterbox WindowLetterbox(GLFWwindow* const Window)
{
    int Width = 0;
    int Height = 0;
    glfwGetWindowSize(Window, &Width, &Height);
    const double Scale = std::min(Width / double(ScreenWidth), Height / double(ScreenHeight));
    return Scale <= 0.0 ? FLetterbox{} : FLetterbox{ .Scale = Scale, .Left = (Width - ScreenWidth * Scale) / 2, .Top = (Height - ScreenHeight * Scale) / 2 };
}

// The mouse in the 640x480 screen, kept inside it as FUN_00451950 keeps the cursor.
std::pair<int32_t, int32_t> ScreenCursor(GLFWwindow* const Window)
{
    const FLetterbox Box = WindowLetterbox(Window);
    double X = 0.0;
    double Y = 0.0;
    glfwGetCursorPos(Window, &X, &Y);
    return {
        std::clamp(int32_t(std::floor((X - Box.Left) / Box.Scale)), 0, int32_t(ScreenWidth) - 1),
        std::clamp(int32_t(std::floor((Y - Box.Top) / Box.Scale)), 0, int32_t(ScreenHeight) - 1),
    };
}

void SetScreenCursor(GLFWwindow* const Window, const int32_t X, const int32_t Y)
{
    constexpr double PixelCentre = 0.5;
    const FLetterbox Box = WindowLetterbox(Window);
    glfwSetCursorPos(Window, Box.Left + (X + PixelCentre) * Box.Scale, Box.Top + (Y + PixelCentre) * Box.Scale);
}

struct FMenuSheets
{
    FSpriteSheet Menu;
    FSpriteSheet VMenu;
};

FMenuSheets CreateMenuSheets(FRenderer& Renderer, const FMenuArt& Art)
{
    return { .Menu = Renderer.CreateSpriteSheet(Art.GetBitmaps(EHudSheet::Menu)), .VMenu = Renderer.CreateSpriteSheet(Art.GetBitmaps(EHudSheet::VMenu)) };
}

// RunMenu at 00421050: steps and draws a menu until an item is chosen, or nullopt when the window closes.
// A menu shown again keeps the cursor where its last run left it (menu +0x20).
std::optional<size_t> RunMenu(GLFWwindow* const Window, FRenderer& Renderer, const FMenuArt& Art, const FMenuSheets& Sheets, const FMenu& Menu, FRandom& Random,
    std::optional<std::pair<int32_t, int32_t>>& Cursor)
{
    FMenuState State = Art.Start(Menu);
    if (Cursor)
    {
        State.CursorX = Cursor->first;
        State.CursorY = Cursor->second;
    }

    SetScreenCursor(Window, State.CursorX, State.CursorY);
    // PgPoll reports presses, so a key held from before the menu does nothing.
    // Escape and Backspace choose the default item, Space and Return the selected one, and the left button the one under the cursor.
    static constexpr std::array<int, 7> Keys = { GLFW_KEY_ESCAPE, GLFW_KEY_BACKSPACE, GLFW_KEY_SPACE, GLFW_KEY_ENTER, GLFW_KEY_KP_ENTER, GLFW_KEY_UP, GLFW_KEY_DOWN };
    constexpr size_t MouseButton = Keys.size();
    const auto ReadDown = [Window]
    {
        std::array<bool, Keys.size() + 1> Down{};
        for (size_t Index = 0; Index < Keys.size(); ++Index)
        {
            Down[Index] = glfwGetKey(Window, Keys[Index]) == GLFW_PRESS;
        }

        Down[MouseButton] = glfwGetMouseButton(Window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        return Down;
    };

    auto Previous = ReadDown();
    std::pair<int32_t, int32_t> Mouse = ScreenCursor(Window);
    FMenuInput Pending{ .MouseX = Mouse.first, .MouseY = Mouse.second };
    const uint32_t FramePeriod = DisplayFramePeriod();
    FFrameClock Clock(FramePeriod);
    FInputPress AnyInput(Window);
    int64_t IdleLeft = Menu.IdleMilliseconds;
    while (!glfwWindowShouldClose(Window))
    {
        PollEvents(Window);
        const bool bAnyInput = AnyInput.Poll();
        const auto Down = ReadDown();
        const auto Pressed = [&](const size_t Index) { return Down[Index] && !Previous[Index]; };
        Pending.bCancel = Pending.bCancel || Pressed(0) || Pressed(1);
        Pending.bAccept = Pending.bAccept || Pressed(2) || Pressed(3) || Pressed(4);
        Pending.bUp = Pending.bUp || Pressed(5);
        Pending.bDown = Pending.bDown || Pressed(6);
        Pending.bClick = Pending.bClick || Pressed(MouseButton);
        Previous = Down;
        const std::pair<int32_t, int32_t> Now = ScreenCursor(Window);
        Pending.bMouseMoved = Pending.bMouseMoved || Now != Mouse;
        Mouse = Now;
        Pending.MouseX = Mouse.first;
        Pending.MouseY = Mouse.second;
        // MenuStep's draw pass counts the idle time down by the frame and starts it again on any input.
        const uint32_t Elapsed = Clock.Poll(ClockMilliseconds());
        IdleLeft = bAnyInput || Pending.bMouseMoved ? int64_t(Menu.IdleMilliseconds) : IdleLeft - Elapsed;
        if (Menu.IdleMilliseconds != 0 && IdleLeft <= 0)
        {
            State.Result = Menu.IdleResult;
        }

        for (uint32_t Remaining = Elapsed; Remaining != 0 && !State.Result;)
        {
            const uint32_t StepMilliseconds = NextGameStep(Remaining, FramePeriod, false);
            const uint32_t Flashes = State.Flashes;
            const std::optional<std::pair<int32_t, int32_t>> Jump = Art.Step(Menu, State, Pending, Random, int32_t(StepMilliseconds));
            if (GameAudio && State.Flashes != Flashes)
            {
                GameAudio->PlayMenuFlash();
            }

            if (Jump)
            {
                SetScreenCursor(Window, Jump->first, Jump->second);
                Mouse = *Jump;
            }

            Pending = { .MouseX = Mouse.first, .MouseY = Mouse.second };
        }

        if (Renderer.BeginFrame())
        {
            for (const FHudSprite& Sprite : Art.Draw(Menu, State))
            {
                DrawHudSprite(Renderer, Sprite.Sheet == EHudSheet::VMenu ? Sheets.VMenu : Sheets.Menu, Sprite);
            }

            Renderer.EndFrame();
        }

        if (State.Result)
        {
            break;
        }
    }

    Cursor = std::pair(State.CursorX, State.CursorY);
    if (GameAudio && State.Result)
    {
        GameAudio->PlayMenuAccept();
    }

    return State.Result;
}

// MenuCreate at 004209b0's rectangle for the menus: from 270, 90 to the screen's bottom right, items 45 pixels apart.
constexpr int32_t MenuLeft = 270;
constexpr int32_t MenuTop = 90;
constexpr int32_t MenuStepY = 45;

// MenuAsk at 004228a0: a question with two answers, 30 pixels right of the menu and from 120 down, its title in font 2. True for the first answer.
// ponytail: MenuExplode at 00421460 sets off explosions over the menu after "Yes, bye.", and the bip sounds wait for audio.
std::optional<bool> Confirm(GLFWwindow* const Window, FRenderer& Renderer, const FMenuArt& Art, const FMenuSheets& Sheets, FRandom& Random, std::string Title,
    std::string Yes, std::string No, const size_t DefaultItem)
{
    constexpr int32_t Indent = 30;
    constexpr int32_t Top = 120;
    constexpr size_t TitleFont = 2;
    const FMenu Menu{
        .Left = MenuLeft + Indent,
        .Top = Top,
        .Right = int32_t(ScreenWidth),
        .Bottom = int32_t(ScreenHeight),
        .StepY = MenuStepY,
        .Title = std::move(Title),
        .TitleFont = TitleFont,
        .Items = { { .Text = std::move(Yes) }, { .Text = std::move(No) } },
        .DefaultItem = DefaultItem,
    };
    std::optional<std::pair<int32_t, int32_t>> Cursor;
    const std::optional<size_t> Item = RunMenu(Window, Renderer, Art, Sheets, Menu, Random, Cursor);
    return Item ? std::optional(*Item == 0) : std::nullopt;
}

constexpr uint32_t MillisecondsPerHour = 3600000;
constexpr uint32_t MillisecondsPerMinute = 60000;

// The difficulty DifficultyName at 00403310 names; the port plays Normal, the original's default.
constexpr std::string_view DifficultyText = "Normal";

// SessionDescribeDefault at 00427ad0: the default session's help text.
std::string SessionHelp(const FSession& Session)
{
    return std::format("{}   ({}:{}:{})\nEnergy : {}\nShield : {}\nDifficulty : {} ...\n", Session.LevelName, Session.Clock / MillisecondsPerHour,
        Session.Clock % MillisecondsPerHour / MillisecondsPerMinute, Session.Clock % MillisecondsPerMinute / MillisecondsPerSecond, int32_t(Session.Energy),
        int32_t(Session.Shield), DifficultyText);
}

// SessionChoose at 00427c30: up to seven saved sessions with their level and clock, then Back, the default. Returns the chosen session.
// ponytail: the session's thumbnail (its .tga, drawn at 8, 150 for the item under the cursor) is not written.
std::optional<size_t> RunSessionMenu(GLFWwindow* const Window, FRenderer& Renderer, const FMenuArt& Art, const FMenuSheets& Sheets, FRandom& Random,
    const std::span<const FSession> Settings)
{
    constexpr int32_t Left = 0x109;
    constexpr int32_t Top = 0x46;
    constexpr int32_t StepY = 0x23;
    constexpr size_t MaxSessions = 7;
    constexpr size_t HighlightFont = 4;
    constexpr size_t ItemFont = 3;
    FMenu Menu{
        .Left = Left,
        .Top = Top,
        .Right = int32_t(ScreenWidth),
        .Bottom = int32_t(ScreenHeight),
        .StepY = StepY,
        .Title = "Saved sessions",
        .HighlightFont = HighlightFont,
        .ItemFont = ItemFont,
    };
    for (const FSession& Session : Settings | std::views::take(MaxSessions))
    {
        Menu.Items.push_back({
            .Text = std::format("{}   [{} h {} mn {} sec] ", Session.LevelName, Session.Clock / MillisecondsPerHour, Session.Clock % MillisecondsPerHour / MillisecondsPerMinute,
                Session.Clock % MillisecondsPerMinute / MillisecondsPerSecond),
            .Help = std::format("Energy : {}\nShield : {}\nDifficulty : {} ...\n", int32_t(Session.Energy), int32_t(Session.Shield), DifficultyText),
        });
    }

    if (Menu.Items.size() < MaxSessions)
    {
        Menu.Items.emplace_back();
    }

    Menu.Items.push_back({ .Text = "Back" });
    Menu.DefaultItem = Menu.Items.size() - 1;
    std::optional<std::pair<int32_t, int32_t>> Cursor = std::pair(int32_t(ScreenWidth) / 2, int32_t(ScreenHeight) / 2);
    const std::optional<size_t> Item = RunMenu(Window, Renderer, Art, Sheets, Menu, Random, Cursor);
    return Item && *Item < std::min(Settings.size(), MaxSessions) ? Item : std::nullopt;
}

// The credits (ShowCredits at 00402990 with the scroller at 00402400): credits.gaf, and credits-en.txt in v-menu.gaf font 4 rising from the bottom of the clip 380, 0 to 639, 479 by half a pixel per step, until it has passed the top or a key or mouse button goes down.
void ShowCredits(GLFWwindow* const Window, FRenderer& Renderer, FResourceArchive& Archive, const FMenuArt& Art)
{
    constexpr int32_t ClipLeft = 380;
    constexpr int32_t ClipRight = 639;
    constexpr int32_t ClipBottom = 479;
    constexpr size_t CreditsFont = 4;
    // The scroller's speed argument, 500, times 0.001.
    constexpr float RisePerStep = 0.5f;
    const std::optional<FObjectGaf> Picture = LoadObjectGaf(Archive, "common\\data\\extra\\credits.gaf");
    const std::optional<std::vector<uint8_t>> TextFile = Archive.Read("common\\data\\extra\\credits-en.txt");
    if (!Picture || Picture->Animations.empty() || Picture->Animations.front().Frames.empty() || !TextFile)
    {
        return;
    }

    const std::string Text(TextFile->begin(), TextFile->end());
    const FSpriteSheet PictureSheet = Renderer.CreateSpriteSheet(Picture->Bitmaps);
    const FSpriteSheet FontSheet = Renderer.CreateSpriteSheet(Art.GetBitmaps(EHudSheet::VMenu));
    const Gaf::FAnimationFrame& Frame = Picture->Animations.front().Frames.front();
    const std::vector<FHudSprite> Lines = Art.DrawPage(CreditsFont, Text, ClipLeft, 0, ClipRight);
    float TextHeight = 0.f;
    for (const FHudSprite& Glyph : Lines)
    {
        const std::vector<Gaf::FFrame>& Bitmaps = Art.GetBitmaps(EHudSheet::VMenu);
        if (Glyph.Frame.BitmapIndex >= 1 && Glyph.Frame.BitmapIndex <= Bitmaps.size())
        {
            TextHeight = std::max(TextHeight, Glyph.Y + float(Bitmaps[Glyph.Frame.BitmapIndex - 1].Height));
        }
    }

    const uint32_t FramePeriod = DisplayFramePeriod();
    FFrameClock Clock(FramePeriod);
    FInputPress Input(Window);
    float Top = float(ClipBottom);
    while (!glfwWindowShouldClose(Window) && Top + TextHeight >= 0.f)
    {
        PollEvents(Window);
        if (Input.Poll())
        {
            break;
        }

        for (uint32_t Remaining = Clock.Poll(ClockMilliseconds()); Remaining != 0;)
        {
            NextGameStep(Remaining, FramePeriod, false);
            Top -= RisePerStep;
        }

        if (Renderer.BeginFrame())
        {
            if (Frame.BitmapIndex != 0)
            {
                Renderer.DrawSprite(PictureSheet, Frame.BitmapIndex - 1, float(int32_t(ScreenWidth) / 2 + Frame.OffsetX), float(int32_t(ScreenHeight) / 2 + Frame.OffsetY));
            }

            for (FHudSprite Glyph : Lines)
            {
                Glyph.Y += float(TruncateLow(Top));
                DrawHudSprite(Renderer, FontSheet, Glyph);
            }

            Renderer.EndFrame();
        }
    }
}

// vz-slide.rsc's slides (slides\slide-001.tga to slide-031.tga) that decode, in order; none without the archive beside vz-data.rsc.
// Failed, when given, counts the slides present that do not decode.
std::vector<Gaf::FFrame> LoadSlides(const std::filesystem::path& DataFolder, size_t* const Failed = nullptr)
{
    constexpr int LastSlide = 0x1f;
    std::optional<std::filesystem::path> SlidePath;
    std::error_code Error;
    for (const std::filesystem::directory_entry& Entry : std::filesystem::directory_iterator(DataFolder, Error))
    {
        std::string Name = Entry.path().filename().string();
        std::ranges::transform(Name, Name.begin(), [](const unsigned char Character) { return char(std::tolower(Character)); });
        if (Name == "vz-slide.rsc")
        {
            SlidePath = Entry.path();
        }
    }

    std::vector<Gaf::FFrame> Slides;
    FResourceArchive Archive;
    if (!SlidePath || !Archive.Open(*SlidePath))
    {
        return Slides;
    }

    for (int Slide = 1; Slide <= LastSlide; ++Slide)
    {
        const std::optional<std::vector<uint8_t>> File = Archive.Read(std::format("slides\\slide-{:03}.tga", Slide));
        if (std::optional<Gaf::FFrame> Image = File ? LoadTga(*File) : std::nullopt)
        {
            Slides.push_back(std::move(*Image));
        }
        else if (File && Failed)
        {
            ++*Failed;
        }
    }

    return Slides;
}

// The mouse wheel's turns since the slide show last looked, which GLFW reports only through a callback.
double SlideScroll = 0.;

// The slide show (ShowSlides at 00404aa0): the slides, each centred for 4 seconds, round again; in the original any key or mouse button ends it.
// The port lets the player step through them: the left button, Right or the wheel turned down go to the next slide, the right button, Left or the wheel turned up back to the one before, and Escape ends it.
// A slide left alone still moves on after 4 seconds.
void ShowSlides(GLFWwindow* const Window, FRenderer& Renderer, const std::vector<Gaf::FFrame>& Slides)
{
    constexpr uint32_t SlideMilliseconds = 4000;
    if (Slides.empty())
    {
        return;
    }

    enum class EStep : uint8_t
    {
        Stay,
        Next,
        Back,
        Exit,
    };
    enum EControl : size_t
    {
        LeftButton,
        RightKey,
        RightButton,
        LeftKey,
        EscapeKey,
        ControlCount,
    };
    std::array<bool, ControlCount> WasDown{};
    const auto ReadStep = [&]
    {
        const std::array<bool, ControlCount> Down = {
            glfwGetMouseButton(Window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS,
            glfwGetKey(Window, GLFW_KEY_RIGHT) == GLFW_PRESS,
            glfwGetMouseButton(Window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS,
            glfwGetKey(Window, GLFW_KEY_LEFT) == GLFW_PRESS,
            glfwGetKey(Window, GLFW_KEY_ESCAPE) == GLFW_PRESS,
        };
        const auto Pressed = [&](const EControl Control) { return Down[Control] && !WasDown[Control]; };
        const double Scroll = std::exchange(SlideScroll, 0.);
        EStep Step = EStep::Stay;
        if (Pressed(EscapeKey))
        {
            Step = EStep::Exit;
        }
        else if (Pressed(LeftButton) || Pressed(RightKey) || Scroll < 0.)
        {
            Step = EStep::Next;
        }
        else if (Pressed(RightButton) || Pressed(LeftKey) || Scroll > 0.)
        {
            Step = EStep::Back;
        }

        WasDown = Down;
        return Step;
    };

    const GLFWscrollfun PreviousScroll = glfwSetScrollCallback(Window, [](GLFWwindow*, double, const double Turn) { SlideScroll += Turn; });
    SlideScroll = 0.;
    ReadStep();
    for (size_t Index = 0; !glfwWindowShouldClose(Window);)
    {
        const Gaf::FFrame& Image = Slides[Index];
        Renderer.ReleaseSpriteSheets();
        const FSpriteSheet Sheet = Renderer.CreateSpriteSheet({ Image });
        const float X = float((int32_t(ScreenWidth) - int32_t(Image.Width)) / 2);
        const float Y = float((int32_t(ScreenHeight) - int32_t(Image.Height)) / 2);
        const uint32_t Start = ClockMilliseconds();
        EStep Step = EStep::Stay;
        while (Step == EStep::Stay && !glfwWindowShouldClose(Window))
        {
            PollEvents(Window);
            Step = ReadStep();
            if (Step == EStep::Stay && ClockMilliseconds() - Start >= SlideMilliseconds)
            {
                Step = EStep::Next;
            }

            if (Renderer.BeginFrame())
            {
                Renderer.DrawSprite(Sheet, 0, X, Y);
                Renderer.EndFrame();
            }
        }

        if (Step == EStep::Exit)
        {
            break;
        }

        Index = Step == EStep::Back ? (Index + Slides.size() - 1) % Slides.size() : (Index + 1) % Slides.size();
    }

    glfwSetScrollCallback(Window, PreviousScroll);
    Renderer.ReleaseSpriteSheets();
}

// Intro.avi beside the archive, whatever its case (IntroPresent at 00403370).
std::optional<std::filesystem::path> FindIntro(const std::filesystem::path& DataFolder)
{
    std::error_code Error;
    for (const std::filesystem::directory_entry& Entry : std::filesystem::directory_iterator(DataFolder, Error))
    {
        std::string Name = Entry.path().filename().string();
        std::ranges::transform(Name, Name.begin(), [](const unsigned char Character) { return char(std::tolower(Character)); });
        if (Name == "intro.avi")
        {
            return Entry.path();
        }
    }

    return std::nullopt;
}

// The intro's file and its container, whose frames point into the file.
struct FIntro
{
    std::vector<uint8_t> File;
    Avi::FMovie Movie;
};

std::unique_ptr<FIntro> LoadIntro(const std::filesystem::path& DataFolder)
{
    const std::optional<std::filesystem::path> Path = FindIntro(DataFolder);
    std::ifstream Stream = Path ? std::ifstream(*Path, std::ios::binary) : std::ifstream();
    if (!Stream)
    {
        return nullptr;
    }

    auto Intro = std::make_unique<FIntro>();
    Intro->File.assign(std::istreambuf_iterator<char>(Stream), {});
    std::optional<Avi::FMovie> Movie = Avi::Read(Intro->File);
    if (!Movie || Movie->VideoCompression != Avi::FourCc("IV50") || Movie->Frames.empty() || Movie->Width == 0 || Movie->Height == 0)
    {
        return nullptr;
    }

    Intro->Movie = std::move(*Movie);
    return Intro;
}

// WAVE_FORMAT_MPEGLAYER3, the intro's sound.
constexpr uint16_t Mp3Format = 0x55;

// PlayIntro at 004033b0: Intro.avi through MCI's avivideo device (AviOpen at 0047c6f0) with the music stopped, until it ends or a key goes down.
// AviOpen fits the video to the screen with its aspect kept and centres it, but puts the right edge at left + width - 5, so it shows 4 pixels narrower: 636x360 at (0, 60).
void PlayIntro(GLFWwindow* const Window, FRenderer& Renderer, const std::filesystem::path& DataFolder)
{
    const std::unique_ptr<FIntro> Intro = LoadIntro(DataFolder);
    if (!Intro)
    {
        std::fprintf(stderr, "Cannot play the intro.\n");
        return;
    }

    const Avi::FMovie& Movie = Intro->Movie;
    const bool bMusic = GameAudio && GameAudio->GetMusicTrack() != 0;
    if (GameAudio)
    {
        GameAudio->RestoreMusic();
        if (std::optional<Sound::FSample> Sound = Movie.SoundFormat == Mp3Format ? Sound::DecodeMp3(Movie.Sound) : std::nullopt)
        {
            GameAudio->PlayMovieSound(std::make_shared<const Sound::FSample>(std::move(*Sound)));
        }
    }

    constexpr int32_t RightEdgeShortfall = 4;
    const bool bFitHeight = ScreenHeight / float(Movie.Height) < ScreenWidth / float(Movie.Width);
    const int32_t FitWidth = bFitHeight ? int32_t(ScreenHeight * float(Movie.Width) / float(Movie.Height)) : int32_t(ScreenWidth);
    const int32_t FitHeight = bFitHeight ? int32_t(ScreenHeight) : int32_t(ScreenWidth * float(Movie.Height) / float(Movie.Width));
    const float Left = float((int32_t(ScreenWidth) - FitWidth) / 2);
    const float Top = float((int32_t(ScreenHeight) - FitHeight) / 2);
    const float ScaleX = float(FitWidth - RightEdgeShortfall) / float(Movie.Width);
    const float ScaleY = float(FitHeight) / float(Movie.Height);

    constexpr uint64_t MicrosecondsPerMillisecond = 1000;
    Indeo5::FDecoder Decoder;
    const FSpriteSheet Sheet = Renderer.CreateVideoSheet(Movie.Width, Movie.Height);
    std::vector<uint32_t> Pixels(size_t{ Movie.Width } * Movie.Height);
    FInputPress Input(Window);
    const uint32_t Start = ClockMilliseconds();
    size_t Decoded = 0;
    while (!glfwWindowShouldClose(Window))
    {
        PollEvents(Window);
        if (Input.Poll())
        {
            break;
        }

        // MCI keeps the picture on time with the sound, dropping frames behind it.
        const size_t Due = size_t(uint64_t(ClockMilliseconds() - Start) * MicrosecondsPerMillisecond / Movie.FrameMicroseconds) + 1;
        if (Due > Movie.Frames.size())
        {
            break;
        }

        bool bNewPicture = false;
        std::expected<void, std::string> Result;
        for (; Decoded < Due && Result; ++Decoded)
        {
            Result = Decoder.Decode(Movie.Frames[Decoded]);
            bNewPicture = bNewPicture || !Movie.Frames[Decoded].empty();
        }

        if (!Result)
        {
            std::fprintf(stderr, "Intro frame %zu: %s\n", Decoded - 1, Result.error().c_str());
            break;
        }

        if (bNewPicture && Decoder.GetWidth() == Movie.Width && Decoder.GetHeight() == Movie.Height)
        {
            Decoder.ConvertToRgba(Pixels);
            Renderer.UpdateVideoSheet(Sheet, Pixels);
        }

        if (Renderer.BeginFrame())
        {
            Renderer.DrawSprite(Sheet, 0, Left, Top, false, false, ESpriteBlend::Opaque, ScaleX, ScaleY);
            Renderer.EndFrame();
        }
    }

    if (GameAudio)
    {
        GameAudio->StopMovieSound();
        if (bMusic)
        {
            GameAudio->StartMusic();
        }
    }

    Renderer.ReleaseSpriteSheets();
}

// Decodes every frame of Intro.avi, checking each band against its checksum, and its sound.
int VerifyIntro(const std::filesystem::path& DataFolder)
{
    const std::unique_ptr<FIntro> Intro = LoadIntro(DataFolder);
    if (!Intro)
    {
        std::printf("Intro: cannot read Intro.avi\n");
        return 1;
    }

    Indeo5::FDecoder Decoder;
    size_t Pictures = 0;
    for (size_t Index = 0; Index < Intro->Movie.Frames.size(); ++Index)
    {
        if (const std::expected<void, std::string> Result = Decoder.Decode(Intro->Movie.Frames[Index]); !Result)
        {
            std::printf("Intro: frame %zu: %s\n", Index, Result.error().c_str());
            return 1;
        }

        Pictures += !Intro->Movie.Frames[Index].empty();
    }

    const std::optional<Sound::FSample> Sound = Intro->Movie.SoundFormat == Mp3Format ? Sound::DecodeMp3(Intro->Movie.Sound) : std::nullopt;
    if (!Sound || Sound->Rate == 0)
    {
        std::printf("Intro: cannot decode the sound\n");
        return 1;
    }

    constexpr double MicrosecondsPerSecond = 1e6;
    std::printf("Intro: %zu frames, %zu pictures, %ux%u, %.1f s of picture and %.1f s of sound\n", Intro->Movie.Frames.size(), Pictures, Decoder.GetWidth(),
        Decoder.GetHeight(), double(Intro->Movie.Frames.size()) * Intro->Movie.FrameMicroseconds / MicrosecondsPerSecond, double(Sound->FrameCount()) / Sound->Rate);
    return 0;
}

// The score table (ScoreShowTable at 00401c40 with ScoreDrawLoop at 00401670): score.gaf, then the table, until a key or mouse button goes down.
// Record, when given, joins vz.sco first (SaveScores at 0043e470 with its second argument set), and its row is brightened.
void ShowScores(GLFWwindow* const Window, FRenderer& Renderer, FResourceArchive& Archive, const FMenuArt& Art, const std::optional<FScoreRecord>& Record)
{
    const std::filesystem::path Path = UserDataDirectory() / "vz.sco";
    FScoreTable Table = ReadScores(Path);
    const size_t Rank = Record ? InsertScore(Table, *Record) : 0;
    if (Rank != 0 && !WriteScores(Path, Table))
    {
        std::fprintf(stderr, "Cannot write %s\n", Path.string().c_str());
    }

    const std::optional<FObjectGaf> Picture = LoadObjectGaf(Archive, "common\\data\\extra\\score.gaf");
    const FSpriteSheet PictureSheet = Picture ? Renderer.CreateSpriteSheet(Picture->Bitmaps) : FSpriteSheet{};
    const FSpriteSheet FontSheet = Renderer.CreateSpriteSheet(Art.GetBitmaps(EHudSheet::VMenu));
    const std::vector<FHudSprite> Sprites = DrawScores(Art, Table, Rank);
    FInputPress Input(Window);
    while (!glfwWindowShouldClose(Window) && !Input.Poll())
    {
        PollEvents(Window);
        if (Renderer.BeginFrame())
        {
            if (Picture && !Picture->Animations.empty() && !Picture->Animations.front().Frames.empty() && Picture->Animations.front().Frames.front().BitmapIndex != 0)
            {
                const Gaf::FAnimationFrame& Frame = Picture->Animations.front().Frames.front();
                Renderer.DrawSprite(PictureSheet, Frame.BitmapIndex - 1, float(int32_t(ScreenWidth) / 2 + Frame.OffsetX), float(int32_t(ScreenHeight) / 2 + Frame.OffsetY));
            }

            for (const FHudSprite& Sprite : Sprites)
            {
                DrawHudSprite(Renderer, FontSheet, Sprite);
            }

            Renderer.EndFrame();
        }
    }
}

// MenuOptions at 004229a0, the options: Video, Audio, Control, User settings and Language, then Back, the default. ponytail: their own menus wait for the settings they change; the port keeps the highest detail preset, the default keys, Normal difficulty and English.
void RunOptionsMenu(GLFWwindow* const Window, FRenderer& Renderer, const FMenuArt& Art, const FMenuSheets& Sheets, FRandom& Random)
{
    const FMenu Menu{
        .Left = MenuLeft,
        .Top = MenuTop,
        .Right = int32_t(ScreenWidth),
        .Bottom = int32_t(ScreenHeight),
        .StepY = MenuStepY,
        .Title = "Options",
        .Items = {
            { .Text = "Video", .Help = "Details, Device, Video Mode, Gamma." },
            { .Text = "Audio", .Help = "Sound driver (DirectSound) is unavalaible." },
            { .Text = "Control", .Help = "Input device, Key settings." },
            { .Text = "User settings", .Help = "User name, Game difficulty level." },
            { .Text = "Language", .Help = "English." },
            { .Text = "Back" },
        },
        .DefaultItem = 5,
    };
    std::optional<std::pair<int32_t, int32_t>> Cursor;
    for (;;)
    {
        const std::optional<size_t> Item = RunMenu(Window, Renderer, Art, Sheets, Menu, Random, Cursor);
        if (!Item || *Item == *Menu.DefaultItem)
        {
            return;
        }
    }
}

// MenuExtras at 00427820, the extras: Scores, Credits, Intro, Slide Show while vz-slide.rsc is there, and Demo, then Back, the default.
// The original's Russian build prints this menu's title and its Scores and Intro items in Russian even in English. The demo is played by the main menu's caller.
bool RunExtrasMenu(GLFWwindow* const Window, FRenderer& Renderer, FResourceArchive& Archive, const FMenuArt& Art, FMenuSheets& Sheets, FRandom& Random,
    const std::filesystem::path& DataFolder)
{
    enum EItem : size_t
    {
        ScoresItem,
        CreditsItem,
        IntroItem,
        SlidesItem,
    };
    // SlidesPresent (00404a60) offers the slide show while vz-slide.rsc is there; the port also needs a slide in it that loads.
    const std::vector<Gaf::FFrame> Slides = LoadSlides(DataFolder);
    const bool bSlides = !Slides.empty();

    FMenu Menu{
        .Left = MenuLeft,
        .Top = MenuTop,
        .Right = int32_t(ScreenWidth),
        .Bottom = int32_t(ScreenHeight),
        .StepY = MenuStepY,
        .Title = "Extras",
        .Items = {
            { .Text = "Scores" },
            { .Text = "Credits" },
            { .Text = "Intro", .Help = FindIntro(DataFolder) ? "Select to view intro animation." : "Intro animation file is not present." },
        },
    };
    if (bSlides)
    {
        Menu.Items.push_back({ .Text = "Slide Show" });
    }

    const size_t DemoItem = Menu.Items.size();
    Menu.Items.push_back({ .Text = "Demo", .Help = "Auto-playing game demo." });
    if (!bSlides)
    {
        Menu.Items.emplace_back();
    }

    Menu.Items.push_back({ .Text = "Back" });
    Menu.DefaultItem = Menu.Items.size() - 1;
    std::optional<std::pair<int32_t, int32_t>> Cursor;
    for (;;)
    {
        const std::optional<size_t> Item = RunMenu(Window, Renderer, Art, Sheets, Menu, Random, Cursor);
        if (!Item || *Item == *Menu.DefaultItem || *Item == DemoItem)
        {
            return Item == DemoItem;
        }

        if (*Item == ScoresItem)
        {
            ShowScores(Window, Renderer, Archive, Art, std::nullopt);
        }
        else if (*Item == CreditsItem)
        {
            ShowCredits(Window, Renderer, Archive, Art);
        }
        else if (*Item == IntroItem && FindIntro(DataFolder))
        {
            PlayIntro(Window, Renderer, DataFolder);
            Sheets = CreateMenuSheets(Renderer, Art);
        }
        else if (*Item == SlidesItem && bSlides)
        {
            ShowSlides(Window, Renderer, Slides);
            Sheets = CreateMenuSheets(Renderer, Art);
        }
    }
}

enum class EMenuChoice : uint8_t
{
    NewGame,
    // Restart Level and Load Game: the session in Chosen.
    Session,
    // Extras' Demo, or 30 seconds of an untouched main menu.
    Demo,
    Quit,
};

// MenuMain at 004271a0, the main menu.
// Options and Extras open nothing yet.
// RandomNext's draws here do not matter, since every level reseeds the generator.
EMenuChoice RunMainMenu(GLFWwindow* const Window, FRenderer& Renderer, FResourceArchive& Archive, const FMenuArt& Art, const FSession* const Session,
    const std::filesystem::path& SessionFolder, const std::filesystem::path& DataFolder, FSession& Chosen)
{
    enum EItem : size_t
    {
        RestartItem,
        NewGameItem,
        LoadItem,
        OptionsItem,
        ExtrasItem,
        ExitItem,
        // Menu +0x58's result when the menu sat idle, which VzMenuLoop takes for the demo.
        IdleDemo = 10,
    };
    std::vector<FSession> Saved = ReadSavedSessions(SessionFolder);
    const auto LoadHelp = [&] { return Saved.empty() ? std::string("No saved session.") : std::format("{} saved session{} available.", Saved.size(), Saved.size() > 1 ? "s" : ""); };
    FMenu Menu{
        .Left = MenuLeft,
        .Top = MenuTop,
        .Right = int32_t(ScreenWidth),
        .Bottom = int32_t(ScreenHeight),
        .StepY = MenuStepY,
        .Items = {
            Session ? FMenuItem{ .Text = "Restart Level", .Help = SessionHelp(*Session) } : FMenuItem{},
            { .Text = "Start New Game" },
            { .Text = "Load Game", .Help = LoadHelp() },
            { .Text = "Options", .Help = "Video, Audio, Control, User settings, Language." },
            { .Text = "Extras", .Help = "Scores, Credits, Intro, Slide Show, Demo." },
            { .Text = "Exit" },
        },
        .DefaultItem = ExitItem,
        .StartItem = Session ? RestartItem : NewGameItem,
        .IdleMilliseconds = 30000,
        .IdleResult = IdleDemo,
    };
    FMenuSheets Sheets = CreateMenuSheets(Renderer, Art);
    FRandom Random;
    std::optional<std::pair<int32_t, int32_t>> Cursor;
    for (;;)
    {
        const std::optional<size_t> Item = RunMenu(Window, Renderer, Art, Sheets, Menu, Random, Cursor);
        if (!Item)
        {
            return EMenuChoice::Quit;
        }

        // VzMenuLoop's case 10 plays the intro before the demo, unless Extras' Demo set session +0x20d0 bit 0x2000, which it then clears.
        if (*Item == IdleDemo)
        {
            if (FindIntro(DataFolder))
            {
                PlayIntro(Window, Renderer, DataFolder);
            }

            return EMenuChoice::Demo;
        }

        if (*Item == RestartItem && Session)
        {
            constexpr size_t NoThanks = 1;
            const std::optional<bool> Retry = Confirm(Window, Renderer, Art, Sheets, Random, "Retry level ?", "Yes please !", "No thanks.", NoThanks);
            if (Retry && *Retry)
            {
                Chosen = *Session;
                return EMenuChoice::Session;
            }

            if (!Retry)
            {
                return EMenuChoice::Quit;
            }
        }
        else if (*Item == NewGameItem)
        {
            return EMenuChoice::NewGame;
        }
        else if (*Item == LoadItem)
        {
            // SessionLoad at 0042ba00 in mode 0 chooses a saved session; back in the menu, the list is read again.
            if (const std::optional<size_t> Index = RunSessionMenu(Window, Renderer, Art, Sheets, Random, Saved))
            {
                Chosen = Saved[*Index];
                return EMenuChoice::Session;
            }

            Saved = ReadSavedSessions(SessionFolder);
            Menu.Items[LoadItem].Help = LoadHelp();
        }
        else if (*Item == OptionsItem)
        {
            RunOptionsMenu(Window, Renderer, Art, Sheets, Random);
        }
        else if (*Item == ExtrasItem)
        {
            if (RunExtrasMenu(Window, Renderer, Archive, Art, Sheets, Random, DataFolder))
            {
                return EMenuChoice::Demo;
            }
        }
        else if (*Item == ExitItem)
        {
            constexpr size_t YesBye = 0;
            const std::optional<bool> Exit = Confirm(Window, Renderer, Art, Sheets, Random, "Exit Now ?", "Yes, bye.", "No !", YesBye);
            if (!Exit || *Exit)
            {
                return EMenuChoice::Quit;
            }
        }
    }
}

// How a level ends: through a lift to NextLevel, back to its start, to the main menu, or with the window closed.
enum class ELevelEnd : uint8_t
{
    Lift,
    Restart,
    Menu,
    Quit,
};

struct FLevelEnd
{
    ELevelEnd Kind = ELevelEnd::Quit;
    std::string NextLevel;
};

// MenuGame at 004223e0, the menu Escape opens during a level: Resume Game, Options, Retry Level (shown while there is a default session, which a running level always has) and End Game.
// Options opens nothing yet.
std::optional<ELevelEnd> RunGameMenu(GLFWwindow* const Window, FRenderer& Renderer, const FMenuArt& Art, const FMenuSheets& Sheets, FRandom& Random)
{
    enum EItem : size_t
    {
        ResumeItem,
        OptionsItem,
        RetryItem,
        EndItem,
    };
    const FMenu Menu{
        .Left = MenuLeft,
        .Top = MenuTop,
        .Right = int32_t(ScreenWidth),
        .Bottom = int32_t(ScreenHeight),
        .StepY = MenuStepY,
        .Items = {
            { .Text = "Resume Game" },
            { .Text = "Options", .Help = "Video, Audio, Control, User settings, Language." },
            { .Text = "Retry Level" },
            { .Text = "End Game" },
        },
        .DefaultItem = ResumeItem,
    };

    std::optional<std::pair<int32_t, int32_t>> Cursor;
    for (;;)
    {
        const std::optional<size_t> Item = RunMenu(Window, Renderer, Art, Sheets, Menu, Random, Cursor);
        if (!Item)
        {
            return ELevelEnd::Quit;
        }

        if (*Item == ResumeItem)
        {
            return std::nullopt;
        }

        if (*Item == OptionsItem)
        {
            RunOptionsMenu(Window, Renderer, Art, Sheets, Random);
        }

        if (*Item == RetryItem)
        {
            constexpr size_t NoThanks = 1;
            const std::optional<bool> Retry = Confirm(Window, Renderer, Art, Sheets, Random, "Retry level ?", "Yes please !", "No thanks.", NoThanks);
            if (!Retry || *Retry)
            {
                return Retry ? ELevelEnd::Restart : ELevelEnd::Quit;
            }
        }
        else if (*Item == EndItem)
        {
            // The first answer is also the default, so Escape twice ends the game.
            constexpr size_t Yes = 0;
            const std::optional<bool> End = Confirm(Window, Renderer, Art, Sheets, Random, "End this Game ?", "Yes", "No", Yes);
            if (!End || *End)
            {
                return End ? ELevelEnd::Menu : ELevelEnd::Quit;
            }
        }
    }
}

// What vzconf.txt sets that the port follows: the session folder, the player's name the saving plots address, and the input device.
struct FSettings
{
    std::filesystem::path Folder;
    std::string PlayerName;
    // use-mouse (keyword 0x124) makes the mouse the input device (session +0x165c = 3) and turns the absolute command on: the ship turns toward the sight, the left button fires, the right one thrusts, Down backs off and Left and Right strafe.
    bool bMouse = false;
    FVideoDetails Details;
    FAudioSettings Audio;
    // gamma = red, green, blue (session +0x27d4): the user's gamma, which every ramp with Gamma FX sits on.
    std::array<int32_t, 3> Gamma{};
    // difficulty-level (keyword 0x102, SetDifficulty at 004032c0) picks one of COMMON\DIFFICULTY.3DC's levels; DifficultyName names them Easy, Normal, Hard and Insane.
    FDifficulty Difficulty;
};

// One level of LoopLevels: runs until the window closes, the player takes a lift (the next level's path, with the player in Carried), or a menu ends or restarts the level.
// A level loaded from a session takes its objects from Restore; any other becomes the default session, in Session and on disk.
std::expected<FLevelEnd, std::string> RunLevel(GLFWwindow* const Window, FRenderer& Renderer, FResourceArchive& Archive, const std::string& LevelPath,
    const std::optional<std::pair<float, float>> Start, const bool bAutoFire, std::optional<FCarriedPlayer>& Carried, const std::span<const std::string> DebugLevels,
    FPanel* const Panel, const FMenuArt* const MenuArt, std::optional<FSession>& Session, const FSession* const Restore, const FSettings& Settings,
    const std::vector<FDemoRecord>* const Demo)
{
    std::expected<FLevel, std::string> Level = LoadLevel(Archive, LevelPath);
    if (!Level)
    {
        return std::unexpected(std::format("Cannot load level: {}", Level.error()));
    }

    if (Restore && Restore->Objects)
    {
        RestoreObjects(*Level, *Restore->Objects);
    }

    const auto PlayerObject = std::ranges::find_if(Level->Objects, [](const FLevelObject& Object) { return Object.Behaviour == "Player"; });
    // LoopLevels puts the carried player object where the new level's player starts; a session's player keeps the level's object.
    if (Carried && !Carried->Object.ClassName.empty() && PlayerObject != Level->Objects.end())
    {
        const float X = PlayerObject->X;
        const float Y = PlayerObject->Y;
        *PlayerObject = Carried->Object;
        PlayerObject->X = X;
        PlayerObject->Y = Y;
    }

    // Level view: MAP_ layers drawn bottom-up with the placed objects.
    // The player drives with the original movement code; levels without a player fall back to scrolling with the arrow keys.
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
    const uint32_t FramePeriod = DisplayFramePeriod();
    std::optional<FCombat> Combat;
    // Session +0x101c: the last step, which the draw pass plays again.
    uint32_t LastStepMilliseconds = 0;
    // The smaller side of the player's current frame, which AnimUpdateBounds makes the body's bounds.
    float PlayerSpriteSize = 0.f;
    std::vector<FObjectBody> Bodies = CreateObjectBodies(*Level);
    const size_t PlayerIndex = size_t(PlayerObject - Level->Objects.begin());
    std::vector<FPickup> Pickups = CreatePickups(*Level);
    std::vector<std::optional<FActorAnimation>> ActorAnimations = CreateActorAnimations(Bodies, *Level, *Objects);
    std::vector<FPlacedAnimation> PlacedAnimations = CreatePlacedAnimations(*Objects, *Level, ActorAnimations);
    std::expected<std::vector<FDoor>, std::string> Doors = CreateDoors(*Level, Archive, Bodies);
    if (!Doors)
    {
        std::fprintf(stderr, "Gates and lifts stay still: %s\n", Doors.error().c_str());
        Doors.emplace();
    }

    std::optional<std::string> NextLevel;
    // The panel's sheets, and one single-pixel frame per radar dot colour: PanelDrawRadar plots palette indices 0xf9 to 0xfc, which ColorMapInit (0046eb60) sets to red, green, yellow and blue from DAT_0051bff8.
    const FSpriteSheet PanelSheet = Panel ? Renderer.CreateSpriteSheet(Panel->GetBitmaps(EHudSheet::Panel)) : FSpriteSheet{};
    const FSpriteSheet VPanelSheet = Panel ? Renderer.CreateSpriteSheet(Panel->GetBitmaps(EHudSheet::VPanel)) : FSpriteSheet{};
    const FSpriteSheet BonusSheet = Panel ? Renderer.CreateSpriteSheet(Panel->GetBitmaps(EHudSheet::Bonus)) : FSpriteSheet{};
    const FSpriteSheet IconSheet = Panel ? Renderer.CreateSpriteSheet(Panel->GetBitmaps(EHudSheet::Icons)) : FSpriteSheet{};
    const FSpriteSheet SurpriseSheet = Panel ? Renderer.CreateSpriteSheet(Panel->GetBitmaps(EHudSheet::Surprise)) : FSpriteSheet{};
    FMessages Messages;
    constexpr std::array<uint16_t, 4> RadarColors = { 0x7c00, 0x03e0, 0x7fe0, 0x001f };
    std::vector<Gaf::FFrame> RadarPixels;
    for (const uint16_t Color : RadarColors)
    {
        RadarPixels.push_back({ .Width = 1, .Height = 1, .ColorKey = uint16_t(~Color & 0x7fff), .Pixels = { Color } });
    }

    const FSpriteSheet ColorSheet = Renderer.CreateSpriteSheet(RadarPixels);
    FPlayerInventory Inventory = Carried ? Carried->Inventory : PlayerObject != Level->Objects.end() ? CreateInventory(*PlayerObject) : FPlayerInventory{};
    // A carried or saved inventory takes the class's absorptions and weight from this level's player.
    if (Carried && PlayerObject != Level->Objects.end())
    {
        FPlayerInventory Class = CreateInventory(*PlayerObject);
        Inventory.Absorptions = std::move(Class.Absorptions);
        Inventory.Weight = Class.Weight;
        Inventory.ShieldUpStep = Class.ShieldUpStep;
    }
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
            if (std::expected<FCombat, std::string> Weapons = CreateCombat(*Level, *PlayerObject, Archive, Bodies, Demo != nullptr))
            {
                Combat = std::move(*Weapons);
                Combat->Details = Settings.Details;
                // EngineActivateObjects applies the difficulty (FUN_0043c4a0) to the player and every hunter as the level starts.
                ApplyDifficulty(Settings.Difficulty, *Combat, Bodies, Inventory);
                // The carried player object brings its weapons, loaded first and in the same order, and the game clock runs on.
                if (Carried)
                {
                    Combat->Clock = Carried->Clock;
                    if (Carried->Weapon.Weapon < Combat->Weapons.size() && Carried->WeaponSlots.size() == Combat->PlayerSlotStates.size())
                    {
                        Combat->PlayerWeapon = Carried->Weapon;
                        Combat->PlayerSlot = Carried->WeaponSlot;
                        Combat->PlayerSlotStates = Carried->WeaponSlots;
                    }
                }

                // DemoStartPlayback at 00429110 starts the game clock at the first record's time.
                if (Demo)
                {
                    Combat->Clock = Demo->front().Time;
                }

                for (const FEffectClass& Effect : Combat->EffectClasses)
                {
                    if (!FindOrLoadSheet(*Objects, Archive, Renderer, Effect.GafPath))
                    {
                        std::fprintf(stderr, "Cannot decode FX GAF %s\n", Effect.GafPath.c_str());
                    }
                }

                for (const FDecorAnimation& Decor : Combat->DecorAnimations)
                {
                    FindOrLoadSheet(*Objects, Archive, Renderer, Decor.GafPath);
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

    // EngineRunLevel writes the default session as the level starts, unless the level came from a session.
    if (!Restore && !Demo)
    {
        Session = FSession{
            .LevelPath = LevelPath,
            .LevelName = Level->Name,
            .Clock = Combat ? Combat->Clock : 0,
            .Energy = Inventory.Energy,
            .Shield = Inventory.Shield,
            .Player = Carried,
        };
        if (!WriteSessionFile(DefaultSessionPath(Settings.Folder), *Session))
        {
            std::fprintf(stderr, "Cannot write the default session in %s\n", Settings.Folder.string().c_str());
        }
    }

    // Session +0x1ff0: the map (action-map), whose picture is made once per level.
    bool bMapShown = false;
    std::optional<FSpriteSheet> MapSheet;
    // MapNeedsCash at 00435fe0.
    // Most of EngineReadActionKeys' lines last a second and stay queued five more.
    constexpr uint32_t VoiceDuration = 1000;
    constexpr uint32_t VoiceHold = 5000;
    const auto SayMapNeedsCash = [&]
    {
        constexpr int32_t TextX = -200;
        constexpr uint32_t LifeMilliseconds = 2000;
        constexpr uint32_t Font = 6;
        Messages.Add({
            .Text = std::format("{},\n   you need MORE Cash\n     to see the Map !", Settings.PlayerName),
            .X = float((int32_t(ScreenWidth) >> 1) + TextX),
            .Y = float(int32_t(ScreenHeight) >> 1),
            .LifeMilliseconds = LifeMilliseconds,
            .Font = Font,
        }, EMessagePlacement::Replace);
        QueueVoice(EVoice::NeedCash, 0, VoiceDuration, VoiceHold);
    };
    // MapToggleWithPanel at 0043b570: MapToggle (00436460) closes the map, or opens it for at least 21000 cash; the panel shows mode 1 under it.
    const auto ToggleMap = [&]
    {
        constexpr float MapCash = 21000.f;
        if (!bMapShown && Inventory.Money < MapCash)
        {
            SayMapNeedsCash();
            return;
        }

        bMapShown = !bMapShown;
        if (bMapShown && !MapSheet)
        {
            MapSheet = Renderer.CreateSpriteSheet({ RenderLevelMap(*Level) });
        }

        if (Panel)
        {
            Panel->ShowMap(bMapShown);
        }
    };
    // Each pass of PanelUpdate in modes 1 to 5 takes 30 cash for the open map, and closes it once the cash runs short, leaving the panel in mode 1.
    const auto PayForMap = [&]
    {
        constexpr int32_t MapCashPerPass = 30;
        if (!bMapShown || !Panel || Panel->GetMode() == 0)
        {
            return;
        }

        if (int32_t(Inventory.Money) < MapCashPerPass)
        {
            bMapShown = false;
            SayMapNeedsCash();
            return;
        }

        // PanelUpdate rings the till each time the cash drops past a multiple of 5000.
        constexpr int32_t TillStep = 5000;
        const int32_t Cash = int32_t(Inventory.Money);
        Inventory.Money -= float(MapCashPerPass);
        if (GameAudio && Cash / TillStep != (Cash - MapCashPerPass) / TillStep)
        {
            GameAudio->PlayMoney();
        }
    };
    // The game's end records its score, from the level's LEVEL-ID [ tower.level ], unless it was the demo.
    const auto ShowFinalScore = [&]
    {
        unsigned Tower = 0;
        unsigned LevelNumber = 0;
        if (!MenuArt || Demo || std::sscanf(Level->Id.c_str(), " %u.%u", &Tower, &LevelNumber) != 2)
        {
            return;
        }

        ShowScores(Window, Renderer, Archive, *MenuArt,
            MakeScore(Tower, LevelNumber, Inventory.Kills, int32_t(Inventory.Earned), Inventory.ShotsHit, Inventory.ShotsMissed, Combat ? Combat->Clock : 0,
                Settings.PlayerName));
    };
    const std::vector<FSavingPlot> SavingPlots = FindSavingPlots(*Level);
    // SessionLoad (0042ba00) in mode 0 leaves a count running, so the plot a saved session starts on does not save again until the player leaves it.
    // The original keeps the done bit from its last save; the port sets it, as after the save that wrote the session.
    FSavingState Saving{ .bCounting = Restore && Restore->Objects, .bDone = Restore && Restore->Objects };
    // The level objects follow their bodies and the player after the steps; a session written during the steps needs them too.
    const auto SyncObjects = [&]
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
    };
    const auto CarriedNow = [&]
    {
        return FCarriedPlayer{
            .Object = *PlayerObject,
            .State = *Player,
            .Inventory = Inventory,
            .Weapon = Combat ? Combat->PlayerWeapon : FWeaponState{},
            .WeaponSlot = Combat ? Combat->PlayerSlot : 1,
            .WeaponSlots = Combat ? Combat->PlayerSlotStates : std::vector<FWeaponState>{},
            .Clock = Combat ? Combat->Clock : 1,
        };
    };
    std::optional<FMenuSheets> MenuSheets;
    bool bEscapeDown = glfwGetKey(Window, GLFW_KEY_ESCAPE) == GLFW_PRESS;
    double CameraX = PlayerObject != Level->Objects.end() ? PlayerObject->X - ScreenWidth * 0.5 : 0.0;
    double CameraY = PlayerObject != Level->Objects.end() ? PlayerObject->Y - ScreenHeight * 0.5 : 0.0;
    std::printf("%s: %s, %zu objects, %zu sprite parts\n", LevelPath.c_str(), Level->Name.c_str(), Level->Objects.size(), Objects->Sprites.size());
    std::fflush(stdout);

    // The finale after a Final-object's death (FUN_00445dd0 with its timer DAT_00523820 and flash timer DAT_00523818): 4.5 seconds, flashing every quarter second until 2.5 are left, then fading out.
    constexpr int32_t FinaleMilliseconds = 0x1194;
    constexpr int32_t FinaleFadeMilliseconds = 0x9c4;
    constexpr int32_t FinaleFlashMilliseconds = 0xfa;
    bool bFinaleStarted = false;
    bool bShowNetworkFailure = false;
    // FUN_0041e5a0 after the game menu closes on a running level: five steps from black, 50 ms apart with Gamma FX and 25 ms with scanlines otherwise, while the game stands still.
    constexpr int MenuFadeSteps = 5;
    int MenuFadeStep = MenuFadeSteps;
    uint32_t MenuFadeTime = 0;
    int32_t FinaleTimer = 0;
    int32_t FinaleFlashTimer = 0;
    bool bFinaleFlash = false;
    // The class's acceleration and turn timeout, which the speed surprises and the speed toggle change.
    const float ClassAcceleration = PlayerClass ? PlayerClass->AccelerationStep : 0.f;
    const int ClassTurnTimeout = PlayerClass ? PlayerClass->TurnTimeout : 0;
    bool bSpeedKeyDown = glfwGetKey(Window, GLFW_KEY_PAGE_DOWN) == GLFW_PRESS;
    FRandom NoMiscRandom;
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
            .bBoost = PlayerClass && PlayerClass->AccelerationStep != ClassAcceleration,
            .bLocked = Combat && Combat->bPlayerLocked,
            .bSurprise = Inventory.Surprises.Active != ESurprise::None,
        };
        for (const FCarriedSurprise& Surprise : Inventory.Surprises.Carried)
        {
            if (Surprise.bStarted)
            {
                Values.SurpriseTimes.push_back(Surprise.Timeout);
            }
        }
        if (Player)
        {
            Values.Speed = Player->SpeedMagnitude;
            Values.Position = Player->Position;
        }

        if (Combat)
        {
            Values.TimeMilliseconds = Combat->Clock;
            Values.WeaponSlot = Combat->PlayerSlot;
            // The weapon in hand's ammo stock (object +0x248); an empty slot holds none.
            if (Combat->PlayerSlot >= 1 && Combat->PlayerSlot <= Combat->PlayerSlotStates.size() && Combat->PlayerWeapon.Weapon < Combat->Weapons.size())
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
    bool bPaused = false;
    // FUN_00445580 at each level start: the level's <level>-<language>.txt, typed over the frozen level until fire or a key, then the level's name rising from the bottom.
    // Only the first level of each tower and the last level have one; a demo shows none.
    std::string PageText;
    if (!Demo)
    {
        if (const std::optional<std::vector<uint8_t>> File = Archive.Read(LevelPath + "-en.txt"))
        {
            PageText.assign(File->begin(), File->end());
        }
    }

    size_t PageTyped = 0;
    FInputPress PageInput(Window);
    bool bPageFireDown = true;
    const auto AnnounceLevel = [&]
    {
        constexpr int32_t NameX = -100;
        constexpr float NameVelocityY = -1.f;
        constexpr uint32_t NameLifeMilliseconds = 5000;
        constexpr uint32_t NameFont = 5;
        constexpr uint32_t NameColor = 0xffe010;
        Messages.Add({
            .Text = Level->Name,
            .X = float((int32_t(ScreenWidth) >> 1) + NameX),
            .Y = float(ScreenHeight),
            .VelocityY = NameVelocityY,
            .LifeMilliseconds = NameLifeMilliseconds,
            .bTypewriter = true,
            .Gaf = EMessageGaf::Bonus,
            .Font = NameFont,
            .Color = { .Diffuse = NameColor },
        }, EMessagePlacement::Replace);
    };
    if (PageText.empty() && !Demo)
    {
        AnnounceLevel();
    }
    bool bPauseKeyDown = glfwGetKey(Window, GLFW_KEY_P) == GLFW_PRESS;
    // The forward input of the last step, which a blocked motor answers with start-engine.wav, and the surprise whose tune plays.
    bool bThrustInput = false;
    bool bFireInput = false;
    ESurprise TuneSurprise = ESurprise::None;
    // The weapon switches already heard; the level's own first selection plays nothing.
    std::optional<uint32_t> HeardWeaponSwitches;
    // Session +0x2090: the shield alarm's repeat timer.
    int32_t ShieldAlarmTimer = 0;
    // EngineApplyDisplayAndCursor (0042f2d0) loads Data\Menu\cursor.gaf's sight (session +0x25b8) for the mouse; EngineRunLevel plays it by the last step and draws it at the mouse after everything else.
    // Any other device hides the cursor altogether.
    const std::optional<FObjectGaf> SightGaf = Settings.bMouse ? LoadObjectGaf(Archive, "common\\data\\menu\\cursor.gaf") : std::nullopt;
    const Gaf::FAnimation* const Sight = SightGaf && !SightGaf->Animations.empty() ? &SightGaf->Animations.front() : nullptr;
    FAnimationPlayback SightPlayback = Sight ? StartAnimation(*Sight) : FAnimationPlayback{};
    std::optional<FSpriteSheet> SightSheet;
    // FUN_00403b70 loads Data\Menu\pause.gaf's first animation, which FUN_00403d70 centres on the screen in draw mode 7.
    const std::optional<FObjectGaf> PauseGaf = LoadObjectGaf(Archive, "common\\data\\menu\\pause.gaf");
    // EngineRunLevel shows Data\Extra\failed.gaf's skull when the player dies: animation 2 (its dark outline, mode 8) under animation 1, centred, until the game ends (FUN_004457f0).
    const std::optional<FObjectGaf> FailedGaf = LoadObjectGaf(Archive, "common\\data\\extra\\failed.gaf");
    std::optional<FSpriteSheet> FailedSheet;
    // A white pixel that the gamma pass stretches over the screen.
    std::optional<FSpriteSheet> WhiteSheet;
    FDeathDrift DeathDrift;
    std::optional<FSpriteSheet> PauseSheet;
    // Backspace, then the digits 1 to 9.
    constexpr size_t WeaponKeyCount = 10;
    // g_DemoReplayCursor.
    size_t DemoCursor = 0;
    // The original replays a recording made on a 60 Hz display, one record per step; a display of another rate would run steps without records.
    // So the port plays the recording's own steps as real time allows, then 10 ms steps once they run out.
    const std::vector<uint32_t> DemoSteps = Demo ? DemoStepLengths(*Demo) : std::vector<uint32_t>();
    size_t DemoStep = 0;
    uint32_t DemoBudget = 0;
    bool bTabDown = glfwGetKey(Window, GLFW_KEY_TAB) == GLFW_PRESS;
    bool bMapKeyDown = glfwGetKey(Window, GLFW_KEY_M) == GLFW_PRESS;
    std::array<bool, WeaponKeyCount> WeaponKeysDown{};
    // Object +0x170, which 3do::CreateObject starts at 50; ObjectDraw counts it up to 49 for a visible object and draws it opaque from there.
    constexpr int FullFade = 49;
    int PlayerFade = FullFade + 1;
    // Is-light objects (class +0x1c bit 0x80000000) stay lit through Darken.
    std::vector<bool> LightObjects(Level->Objects.size());
    for (size_t Index = 0; Index < Level->Objects.size(); ++Index)
    {
        LightObjects[Index] = ParseLevelFloat(Level->Objects[Index].Properties.Find("Is-light")).value_or(0.f) != 0.f;
    }

    std::optional<FObjectGaf> DarkZone;
    std::optional<FSpriteSheet> DarkSheet;
    const auto DrawDarkness = [&](const FPlayerState& Viewer, const int OriginX, const int OriginY)
    {
        constexpr int DarkWidth = 0x178;
        constexpr int DarkHeight = 0x135;
        // The window starts 8 pixels left and 16 up of centring on the player, inset by 2 on each side.
        constexpr int DarkShiftX = 8;
        constexpr int DarkShiftY = 16;
        constexpr int DarkInset = 2;
        const int Left = int(SpriteCoordinate(Viewer.Position.X)) - OriginX - DarkWidth / 2 - DarkShiftX + DarkInset;
        const int Top = int(SpriteCoordinate(Viewer.Position.Y)) - OriginY - DarkHeight / 2 - DarkShiftY + DarkInset;
        const int Right = Left + DarkWidth - 2 * DarkInset;
        const int Bottom = Top + DarkHeight - 2 * DarkInset;
        if (!WhiteSheet)
        {
            constexpr uint16_t White = 0x7fff;
            WhiteSheet = Renderer.CreateSpriteSheet({ Gaf::FFrame{ .Width = 1, .Height = 1, .Pixels = { White } } });
        }

        constexpr uint32_t Black = 0;
        const auto Fill = [&](const int X0, const int Y0, const int X1, const int Y1)
        {
            if (X1 > X0 && Y1 > Y0)
            {
                Renderer.DrawSprite(*WhiteSheet, 0, float(X0), float(Y0), false, false, ESpriteBlend::Opaque, float(X1 - X0), float(Y1 - Y0), UINT32_MAX, Black);
            }
        };
        const int Width = int(ScreenWidth);
        const int Height = int(ScreenHeight);
        Fill(0, 0, Width, std::max(Top, 0));
        Fill(0, std::min(Bottom, Height), Width, Height);
        Fill(0, std::max(Top, 0), std::max(Left, 0), std::min(Bottom, Height));
        Fill(std::min(Right, Width), std::max(Top, 0), Width, std::min(Bottom, Height));

        if (!DarkZone)
        {
            DarkZone = LoadObjectGaf(Archive, "common\\data\\object\\fx\\darkzone.gaf");
            if (DarkZone)
            {
                DarkSheet = Renderer.CreateSpriteSheet(DarkZone->Bitmaps);
            }
        }

        if (!DarkZone || !DarkSheet || DarkZone->Animations.empty() || DarkZone->Animations.front().Frames.empty() || DarkZone->Animations.front().Frames.front().BitmapIndex == 0)
        {
            return;
        }

        const Gaf::FAnimationFrame& Frame = DarkZone->Animations.front().Frames.front();
        const Gaf::FFrame& Bitmap = DarkZone->Bitmaps[Frame.BitmapIndex - 1];
        const float QuarterWidth = float(DarkWidth / 2);
        const float QuarterHeight = float(DarkHeight / 2);
        const float ScaleX = QuarterWidth / float(Bitmap.Width);
        const float ScaleY = QuarterHeight / float(Bitmap.Height);
        for (int Quarter = 0; Quarter < 4; ++Quarter)
        {
            const bool bRight = (Quarter & 1) != 0;
            const bool bLower = (Quarter & 2) != 0;
            // A mirrored sprite extends back from its anchor, so a mirrored quarter is anchored at its far edge.
            Renderer.DrawSprite(*DarkSheet, Frame.BitmapIndex - 1, float(Left) + (bRight ? 2.f * QuarterWidth : 0.f), float(Top) + (bLower ? 2.f * QuarterHeight : 0.f),
                bRight, bLower, FrameBlend(Frame), ScaleX, ScaleY);
        }
    };
    // The same counter for the other objects, by object index; invisible enemies fade to nothing, and ObjectActivate (0043c150) starts them there.
    std::vector<int> ObjectFades(Level->Objects.size(), FullFade + 1);
    for (const FObjectBody& Body : Bodies)
    {
        if (Body.bInvisible)
        {
            ObjectFades[Body.ObjectIndex] = 0;
        }
    }
    // Object +0xcc of the dead player: EngineUpdateObjects ends the game when it runs out.
    constexpr int32_t DeathMilliseconds = 10000;
    std::optional<int32_t> DeathTimer;
    const auto Current = std::ranges::find(DebugLevels, LevelPath);
    const size_t CurrentIndex = Current == DebugLevels.end() ? DebugLevels.size() : size_t(Current - DebugLevels.begin());
    while (!glfwWindowShouldClose(Window))
    {
        PollEvents(Window);
        // EnginePause (0043b8d0): action-pause (P) closes an open map, or freezes the game (session +0x20 bit 0x1000000) under pause.gaf until P again; meanwhile only P is read and the clock's time is dropped.
        const bool bPausePressed = glfwGetKey(Window, GLFW_KEY_P) == GLFW_PRESS && !bPauseKeyDown;
        bPauseKeyDown = glfwGetKey(Window, GLFW_KEY_P) == GLFW_PRESS;
        if (bPausePressed && Player && !Demo && !bFinaleStarted)
        {
            QueueVoice(EVoice::Pause, 0, VoiceDuration, VoiceHold);
            if (bMapShown)
            {
                ToggleMap();
            }
            else
            {
                bPaused = !bPaused;
            }
        }

        // FUN_004282a0 types one character per frame; fire while typing ends the page at once, and once typed, fire or any key does.
        if (!PageText.empty())
        {
            const bool bAnyPressed = PageInput.Poll();
            const bool bFireDown = glfwGetKey(Window, GLFW_KEY_SPACE) == GLFW_PRESS || glfwGetKey(Window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                glfwGetMouseButton(Window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            const bool bFirePressed = bFireDown && !bPageFireDown;
            bPageFireDown = bFireDown;
            if (bFirePressed || (PageTyped >= PageText.size() && bAnyPressed))
            {
                PageText.clear();
                AnnounceLevel();
            }
            else if (PageTyped < PageText.size())
            {
                ++PageTyped;
            }
        }

        if (!bPaused && PageText.empty())
        {
            // Escape (EngineEscape at 00445b40) opens the game menu, or ends the game at once for a dead player; the menu's steps do not count as game time.
            const bool bEscapePressed = glfwGetKey(Window, GLFW_KEY_ESCAPE) == GLFW_PRESS && !bEscapeDown;
            bEscapeDown = glfwGetKey(Window, GLFW_KEY_ESCAPE) == GLFW_PRESS;
            // A playing demo ends on Escape (EngineRunLevel checks the key before DemoReplayStep).
            if (bEscapePressed && Demo)
            {
                return FLevelEnd{ .Kind = ELevelEnd::Menu };
            }

            if (bEscapePressed && bMapShown)
            {
                ToggleMap();
            }
            else if (bEscapePressed && DeathTimer)
            {
                *DeathTimer = 0;
            }
            else if (bEscapePressed && MenuArt && !bFinaleStarted)
            {
                if (!MenuSheets)
                {
                    MenuSheets = CreateMenuSheets(Renderer, *MenuArt);
                }

                // EngineReadActionKeys says Menu, at once, while the player lives.
                if (Player && Inventory.Energy > 0.f && QueueVoice(EVoice::Menu, 0, VoiceDuration, 2000) && GameAudio)
                {
                    GameAudio->UpdateVoices();
                }

                FRandom MenuRandom;
                const std::optional<ELevelEnd> End = RunGameMenu(Window, Renderer, *MenuArt, *MenuSheets, Combat ? Combat->Random : MenuRandom);
                if (End)
                {
                    return FLevelEnd{ .Kind = *End };
                }

                // MenuGame fades the menu out with fade.wav, then the level in. ponytail: the menu's own fade to black is left out; it closes at once.
                MenuFadeStep = 0;
                MenuFadeTime = ClockMilliseconds();
                if (GameAudio)
                {
                    GameAudio->PlayFade();
                }

                bEscapeDown = glfwGetKey(Window, GLFW_KEY_ESCAPE) == GLFW_PRESS;
                Clock = FFrameClock(FramePeriod);
                Clock.Poll(ClockMilliseconds());
            }

            const bool bNextPressed = glfwGetKey(Window, GLFW_KEY_N) == GLFW_PRESS && !bNextKeyDown;
            bNextKeyDown = glfwGetKey(Window, GLFW_KEY_N) == GLFW_PRESS;
            if (bNextPressed && !DebugLevels.empty())
            {
                const size_t Next = (CurrentIndex + 1) % DebugLevels.size();
                std::printf("Debug: level %zu of %zu\n", Next + 1, DebugLevels.size());
                Carried.reset();
                return FLevelEnd{ .Kind = ELevelEnd::Lift, .NextLevel = DebugLevels[Next] };
            }

            // EngineReadActionKeys (0043b5c0) with the default keys of DAT_00509298: Backspace is action-next-weapon and 1 to 9 are action-weapon-1 to 9 (FUN_0043bad0 skips the slot in hand); Tab is action-cycle-panels, Left Shift stepping back, and M is action-map.
            const bool bTabPressed = glfwGetKey(Window, GLFW_KEY_TAB) == GLFW_PRESS && !bTabDown;
            bTabDown = glfwGetKey(Window, GLFW_KEY_TAB) == GLFW_PRESS;
            if (bTabPressed && Panel && !Demo && !bFinaleStarted)
            {
                Panel->CyclePanels(glfwGetKey(Window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS);
            }

            // action-toggle-hilo-speed (Page Down, DIK_NEXT in DAT_00509298), FUN_0043b830: at full acceleration or more the player drops to a third of the class's and turns 1.5 times slower ("low speed"); otherwise both return to the class's ("high speed").
            const bool bSpeedPressed = glfwGetKey(Window, GLFW_KEY_PAGE_DOWN) == GLFW_PRESS && !bSpeedKeyDown;
            bSpeedKeyDown = glfwGetKey(Window, GLFW_KEY_PAGE_DOWN) == GLFW_PRESS;
            if (bSpeedPressed && Player && PlayerClass && !Demo && !bFinaleStarted)
            {
                constexpr float LowSpeedScale = 1.f / 3.f;
                constexpr double LowSpeedTurnScale = 1.5f;
                constexpr uint32_t SpeedVoiceDuration = 1000;
                constexpr uint32_t SpeedVoiceHold = 2000;
                if (PlayerClass->AccelerationStep >= ClassAcceleration)
                {
                    PlayerClass->AccelerationStep = ClassAcceleration * LowSpeedScale;
                    PlayerClass->TurnTimeout = TruncateLow(double(PlayerClass->TurnTimeout) * LowSpeedTurnScale);
                    QueueVoice(EVoice::LowSpeed, 0, SpeedVoiceDuration, SpeedVoiceHold);
                }
                else
                {
                    PlayerClass->AccelerationStep = ClassAcceleration;
                    PlayerClass->TurnTimeout = ClassTurnTimeout;
                    QueueVoice(EVoice::HighSpeed, 0, SpeedVoiceDuration, SpeedVoiceHold);
                }
            }

            const bool bMapPressed = glfwGetKey(Window, GLFW_KEY_M) == GLFW_PRESS && !bMapKeyDown;
            bMapKeyDown = glfwGetKey(Window, GLFW_KEY_M) == GLFW_PRESS;
            if (bMapPressed && Player && !Demo && !bFinaleStarted)
            {
                ToggleMap();
                QueueVoice(EVoice::Map, 0, VoiceDuration, VoiceHold);
            }

            std::array<bool, WeaponKeyCount> WeaponKeysNow{};
            WeaponKeysNow[0] = glfwGetKey(Window, GLFW_KEY_BACKSPACE) == GLFW_PRESS;
            for (size_t Slot = 1; Slot < WeaponKeyCount; ++Slot)
            {
                WeaponKeysNow[Slot] = glfwGetKey(Window, GLFW_KEY_0 + int(Slot)) == GLFW_PRESS;
            }

            if (Combat && !Demo)
            {
                if (WeaponKeysNow[0] && !WeaponKeysDown[0])
                {
                    SelectPlayerWeapon(*Combat, NextPlayerWeaponSlot(*Combat, Inventory), Inventory, Messages);
                }

                for (size_t Slot = 1; Slot < WeaponKeysNow.size(); ++Slot)
                {
                    if (WeaponKeysNow[Slot] && !WeaponKeysDown[Slot] && Combat->PlayerSlot != Slot && !bFinaleStarted)
                    {
                        SelectPlayerWeapon(*Combat, Slot, Inventory, Messages);
                    }
                }
            }

            WeaponKeysDown = WeaponKeysNow;
        }

        uint32_t RemainingMilliseconds = Clock.Poll(ClockMilliseconds());
        if (bPaused || !PageText.empty() || MenuFadeStep < MenuFadeSteps)
        {
            RemainingMilliseconds = 0;
        }

        if (DemoStep < DemoSteps.size())
        {
            DemoBudget += RemainingMilliseconds;
            RemainingMilliseconds = 0;
        }

        const auto NextStep = [&]() -> std::optional<uint32_t>
        {
            if (DemoStep < DemoSteps.size())
            {
                if (DemoBudget < DemoSteps[DemoStep])
                {
                    return std::nullopt;
                }

                DemoBudget -= DemoSteps[DemoStep];
                return DemoSteps[DemoStep++];
            }

            return RemainingMilliseconds != 0 ? std::optional(NextGameStep(RemainingMilliseconds, FramePeriod, Demo != nullptr)) : std::nullopt;
        };
        while (const std::optional<uint32_t> NextStepMilliseconds = NextStep())
        {
            const uint32_t StepMilliseconds = *NextStepMilliseconds;
            // DemoReplayStep (004291d0) takes the next record's input once the game clock reaches its time, staying on the last record; then EngineReplayActionKeys (0043bb00) replays its actions, of which 0x8000 (action-menu) ends the demo.
            uint32_t DemoInput = 0;
            if (Demo && Combat && Player)
            {
                if (Combat->Clock >= (*Demo)[DemoCursor].Time)
                {
                    DemoInput = (*Demo)[DemoCursor].Input;
                    DemoCursor = std::min(DemoCursor + 1, Demo->size() - 1);
                }

                constexpr uint32_t NextWeaponBit = 0x100;
                constexpr uint32_t MapBit = 0x400;
                constexpr uint32_t CyclePanelsBit = 0x4000;
                constexpr uint32_t MenuBit = 0x8000;
                constexpr uint32_t FirstWeaponBit = 0x10000;
                if ((DemoInput & CyclePanelsBit) != 0 && Panel)
                {
                    Panel->CyclePanels(false);
                }

                if ((DemoInput & MapBit) != 0)
                {
                    ToggleMap();
                }

                if ((DemoInput & NextWeaponBit) != 0)
                {
                    SelectPlayerWeapon(*Combat, NextPlayerWeaponSlot(*Combat, Inventory), Inventory, Messages);
                }

                for (size_t Slot = 1; Slot < WeaponKeyCount; ++Slot)
                {
                    if ((DemoInput & (FirstWeaponBit << (Slot - 1))) != 0)
                    {
                        SelectPlayerWeapon(*Combat, Slot, Inventory, Messages);
                        break;
                    }
                }

                if ((DemoInput & MenuBit) != 0)
                {
                    return FLevelEnd{ .Kind = ELevelEnd::Menu };
                }
            }

            const auto Pressed = [Window](const int Key) { return glfwGetKey(Window, Key) == GLFW_PRESS; };
            if (Player)
            {
                // EngineUpdateObjects clears the lock before its objects update, dead player or not; nothing locks onto a dead player again.
                if (Combat)
                {
                    Combat->bPlayerLocked = false;
                }

                // The dead player's object skips its updates until the timer ends the game (game update result 0).
                if (DeathTimer)
                {
                    *DeathTimer -= int32_t(StepMilliseconds);
                    if (*DeathTimer <= 0)
                    {
                        break;
                    }

                    StepDeathDrift(DeathDrift, *Player, *Level, StepMilliseconds);
                }
                else
                {
                    // The player's update starts with its surprises (FUN_0041d290).
                    if (Combat)
                    {
                        StepSurprises(*Combat, *Level, Bodies, *Player, *PlayerClass, Inventory, Messages, StepMilliseconds);
                    }

                    StepCodeTimeouts(Inventory, *Level, Messages, StepMilliseconds);

                    // ReadPlayerInput: Reversed-commands swaps left with right and the two strafes; Blocked-motor ignores every command.
                    // The default strafes are Left Shift with an arrow, which then does not turn (InputActionPressed at 0041d4c0); A and D strafe too.
                    // With the mouse as device (DAT_00509368's defaults) Left and Right strafe on their own, the right button thrusts (Up too), and the turn comes from the sight unless that side strafes.
                    const FSurpriseState& Surprises = Inventory.Surprises;
                    const bool bShift = Pressed(GLFW_KEY_LEFT_SHIFT) || Settings.bMouse;
                    const bool bLeftKey = Pressed(GLFW_KEY_LEFT);
                    const bool bRightKey = Pressed(GLFW_KEY_RIGHT);
                    const bool bSideLeft = (bLeftKey && bShift) || Pressed(GLFW_KEY_A);
                    const bool bSideRight = (bRightKey && bShift) || Pressed(GLFW_KEY_D);
                    bool bTurnLeft = bLeftKey && !bShift;
                    bool bTurnRight = bRightKey && !bShift;
                    if (Settings.bMouse)
                    {
                        const std::pair<int32_t, int32_t> Mouse = ScreenCursor(Window);
                        const FCollisionPoint ToSight{
                            .X = float(Mouse.first - (TruncateLow(Player->Position.X) - int32_t(std::floor(CameraX)))),
                            .Y = float(Mouse.second - (TruncateLow(Player->Position.Y) - int32_t(std::floor(CameraY)))),
                        };
                        const EPlayerTurn Turn = TurnTowardSight(*Player, ToSight);
                        bTurnLeft = Turn == EPlayerTurn::Left && !bSideLeft;
                        bTurnRight = Turn == EPlayerTurn::Right && !bSideRight;
                    }

                    // Extasy (object flag 0x1000000) holds the thrust full forward whatever the keys say.
                    const bool bForward = Surprises.Active == ESurprise::Extasy || Pressed(GLFW_KEY_UP) ||
                        (Settings.bMouse && glfwGetMouseButton(Window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS);
                    bThrustInput = bForward;
                    const bool bLeft = Surprises.bReversed ? bTurnRight : bTurnLeft;
                    const bool bRight = Surprises.bReversed ? bTurnLeft : bTurnRight;
                    const bool bStrafeLeft = Surprises.bReversed ? bSideRight : bSideLeft;
                    const bool bStrafeRight = Surprises.bReversed ? bSideLeft : bSideRight;
                    FPlayerCommand Command = Surprises.bBlockedMotor ? FPlayerCommand{}
                                                                      : PlayerCommandFromKeys(bForward, Pressed(GLFW_KEY_DOWN) && Surprises.Active != ESurprise::Extasy, bLeft, bRight,
                                                                          bStrafeLeft, bStrafeRight);
                    // ReplayPlayerInput (00438800) puts the player where the record before the cursor has it, facing its way, with the move as its velocity in collision units, and drives with the replayed input.
                    if (Demo && DemoCursor >= 1)
                    {
                        const FDemoRecord& Placed = (*Demo)[DemoCursor - 1];
                        if (Player->Position.X != Placed.X || Player->Position.Y != Placed.Y)
                        {
                            Player->Position = { .X = Placed.X, .Y = Placed.Y };
                            Player->Center = CollisionFromWorld(Player->Position);
                            const FCollisionPoint Before = CollisionFromWorld(Player->PreviousPosition);
                            Player->Velocity = { .X = Player->Center.X - Before.X, .Y = Player->Center.Y - Before.Y };
                        }

                        if (Placed.Direction >= 0 && Placed.Direction < ObjectDirectionCount)
                        {
                            Player->Direction = Placed.Direction;
                            Player->Pose = Player->TargetPose = Placed.Direction * (PlayerPoseCount / ObjectDirectionCount) + 1;
                        }

                        Command = PlayerCommandFromInput(DemoInput);
                        bThrustInput = (DemoInput & PlayerInputForward) != 0;
                    }

                    if (const FObjectBody* const Struck = StepPlayer(*Player, *PlayerClass, Command, StepMilliseconds, *Terrain, *Level, Bodies))
                    {
                        ReportDoorContact(*Doors, *Struck, *Player, Inventory, Messages);
                    }

                    // EngineUpdateObjects puts the sound listener where the player stands (SoundSetListener at 0045edb0).
                    SetSoundListener(Player->Position.X, Player->Position.Y, PlayerObject->Altitude);
                    if (Combat)
                    {
                        SetVoiceClock(Combat->Clock);
                        StepPlayerVoices(Combat->Clock, bFireInput);
                    }
                }

                StepObjectBodies(Bodies, StepMilliseconds, *Terrain, *Level);
                if (!DeathTimer)
                {
                    StepShields(Bodies, *Level, Inventory);
                }

                const FPlayerTarget Rider{ .State = *Player, .Radius = PlayerClass->Radius, .Inventory = Inventory, .ObjectIndex = PlayerIndex };
                StepActors(Combat ? &*Combat : nullptr, Bodies, StepMilliseconds, *Terrain, *Level, &Rider);
                if (const FDoor* const Lift = StepDoors(*Doors, *Level, Bodies, Rider, StepMilliseconds, Messages); Lift && !NextLevel && !DeathTimer)
                {
                    // A starred 3do-file (only "*End of Tower 3*" in the campaign) ends the game instead of loading a level.
                    NextLevel = Lift->NextLevel.starts_with('*') ? Lift->NextLevel : NextLevelPath(LevelPath, *Lift);
                    // ObjectUpdateLevelExit drops the queued lines and says Well Done.
                    constexpr uint32_t WellDoneHold = 10000;
                    ClearVoices();
                    if (QueueVoice(EVoice::WellDone, 0, VoiceDuration, WellDoneHold) && GameAudio)
                    {
                        GameAudio->UpdateVoices();
                    }
                }
                if (Combat)
                {
                    const float MoneyBefore = Inventory.Money;
                    Combat->Clock += StepMilliseconds;
                    const float EnergyBefore = Inventory.Energy;
                    const float ShieldBefore = Inventory.Shield;
                    StepGenerators(*Combat, *Level, Bodies, Rider, StepMilliseconds);
                    constexpr uint32_t FireBit = 0x2;
                    const bool bMouseFire = Settings.bMouse && glfwGetMouseButton(Window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
                    const bool bLiveFire = Demo ? (DemoInput & FireBit) != 0 : bAutoFire || bMouseFire || Pressed(GLFW_KEY_SPACE) || Pressed(GLFW_KEY_LEFT_CONTROL);
                    bFireInput = bLiveFire;
                    const bool bFire = !Inventory.Surprises.bBlockedMotor && bLiveFire;
                    if (!DeathTimer)
                    {
                        StepPlayerWeapon(*Combat, bFire, *Player, *PlayerObject, PlayerIndex, PlayerSpriteSize, Inventory, Messages);
                    }

                    StepTurrets(*Combat, StepMilliseconds, *Terrain, *Level, Bodies, *Player);
                    StepProjectiles(*Combat, StepMilliseconds, *Terrain, *Level, Bodies,
                        { .State = *Player, .Radius = PlayerClass->Radius, .Inventory = Inventory, .ObjectIndex = PlayerIndex }, Messages);
                    Player->Velocity = { .X = Player->Velocity.X + Inventory.Shove.X, .Y = Player->Velocity.Y + Inventory.Shove.Y };
                    Inventory.Shove = {};
                    // EngineRunLevel each step: once the fast-kill window closes with the player alive, two or more kills in it pay ((kills - 2) * 5 + 10) * 1000, say so for 8 seconds in red v-bonus font 5, fading, and cheer by the game clock.
                    if (Combat->FastKillTimer > int32_t(StepMilliseconds))
                    {
                        Combat->FastKillTimer -= int32_t(StepMilliseconds);
                    }
                    else if (Inventory.Energy > 0.f)
                    {
                        Combat->FastKillTimer = 0;
                        if (Combat->FastKillCount > 1)
                        {
                            constexpr int32_t BonusX = 100;
                            constexpr int32_t BonusRise = 0x66;
                            constexpr uint32_t BonusLifeMilliseconds = 8000;
                            constexpr uint32_t BonusFont = 5;
                            constexpr uint32_t BonusColor = 0xff2010;
                            constexpr int32_t BonusBlinkMilliseconds = 10;
                            const int32_t Bonus = ((Combat->FastKillCount - 2) * 5 + 10) * 1000;
                            Inventory.Money += float(Bonus);
                            Inventory.Earned += float(Bonus);
                            Messages.Add({
                                .Text = std::format("Well done {} !!!\nFast Kill Bonus +{} $", Settings.PlayerName, Bonus),
                                .X = float(BonusX),
                                .Y = float(int32_t(ScreenHeight) / 2 - BonusRise),
                                .LifeMilliseconds = BonusLifeMilliseconds,
                                .bTypewriter = true,
                                .Gaf = EMessageGaf::Bonus,
                                .Font = BonusFont,
                                .Color = { .Diffuse = BonusColor },
                                .BlinkPeriod = BonusBlinkMilliseconds,
                                .bFade = true,
                            }, EMessagePlacement::Always);
                            constexpr std::array FastKillVoices = { EVoice::WellDone, EVoice::Yeepee, EVoice::ComeOn, EVoice::Ariba, EVoice::Laugh, EVoice::TheBest, EVoice::WantMore,
                                EVoice::Hey };
                            constexpr uint32_t CheerDelay = 500;
                            constexpr uint32_t CheerDuration = 1000;
                            constexpr uint32_t CheerHold = 5000;
                            QueueVoice(FastKillVoices[Combat->Clock % FastKillVoices.size()], CheerDelay, CheerDuration, CheerHold);
                        }

                        Combat->FastKillCount = 0;
                    }

                    // FUN_0043db70 starts the finale: the motor channel stops and Yeepee replaces the queued lines. With Gamma FX each flash plays electric.wav.
                    if (Combat->bFinale && !bFinaleStarted)
                    {
                        bFinaleStarted = true;
                        FinaleTimer = FinaleMilliseconds;
                        FinaleFlashTimer = FinaleFlashMilliseconds;
                        bFinaleFlash = true;
                        constexpr uint32_t YeepeeHold = 10000;
                        ClearVoices();
                        if (QueueVoice(EVoice::Yeepee, 0, VoiceDuration, YeepeeHold) && GameAudio)
                        {
                            GameAudio->UpdateVoices();
                        }

                        if (GameAudio)
                        {
                            GameAudio->StopMotor();
                            if (Settings.Details.bGammaFx)
                            {
                                GameAudio->PlayFinaleFlash();
                            }
                        }
                    }
                    else if (FinaleTimer > 0 && Settings.Details.bGammaFx && FinaleTimer >= FinaleFadeMilliseconds)
                    {
                        FinaleFlashTimer -= int32_t(StepMilliseconds);
                        if (FinaleFlashTimer < 1)
                        {
                            FinaleFlashTimer = FinaleFlashMilliseconds;
                            bFinaleFlash = true;
                            if (GameAudio)
                            {
                                GameAudio->PlayFinaleFlash();
                            }
                        }
                    }

                    if (FinaleTimer > 0)
                    {
                        FinaleTimer -= int32_t(StepMilliseconds);
                        if (FinaleTimer <= 0 && !NextLevel)
                        {
                            bShowNetworkFailure = true;
                            NextLevel = "*";
                        }
                    }
                    if (Inventory.Money != MoneyBefore || Inventory.Energy != EnergyBefore || Inventory.Shield != ShieldBefore)
                    {
                        std::printf("Energy %.0f, shield %.0f, money %.0f\n", Inventory.Energy, Inventory.Shield, Inventory.Money);
                        std::fflush(stdout);
                    }

                    // Spent energy kills the player like any object: its explosions go off and the object stays hidden.
                    if (!DeathTimer && Inventory.Energy <= 0.f)
                    {
                        ExplodePlayer(*Combat, Player->Position, PlayerObject->Altitude);
                        PlayerObject->bRemoved = true;
                        DeathTimer = DeathMilliseconds;
                        Player->bDying = true;
                        if (GameAudio)
                        {
                            GameAudio->PlayDeath();
                            GameAudio->StopMotor();
                        }

                        // With Gamma FX the screen turns red until the game ends.
                        constexpr int32_t DeathRed = 0x50;
                        if (Settings.Details.bGammaFx)
                        {
                            Inventory.Surprises.Gamma = { DeathRed, 0, 0 };
                        }
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

                    ActorAnimations.resize(Level->Objects.size());
                    for (const size_t Created : Combat->CreatedObjects)
                    {
                        const auto Body = std::ranges::find(Bodies, Created, &FObjectBody::ObjectIndex);
                        ActorAnimations[Created] = Body == Bodies.end() ? std::nullopt : CreateActorAnimation(*Body, *Level, *Objects);
                    }

                    Combat->CreatedObjects.clear();
                }

                if (!DeathTimer && CollectPickups(*Player, *PlayerClass, Inventory, Pickups, *Level, Messages, Combat ? &*Combat : nullptr) != 0)
                {
                    if (GameAudio)
                    {
                        GameAudio->PlayPickup();
                    }

                    std::printf("Energy %.0f, shield %.0f, money %.0f\n", Inventory.Energy, Inventory.Shield, Inventory.Money);
                    std::fflush(stdout);
                }

                const float CashBeforePlot = Inventory.Money;
                if (StepSavingPlots(Saving, SavingPlots, *Level, *Player, PlayerClass->Radius, Inventory, Messages, ClockMilliseconds(), Settings.PlayerName))
                {
                    if (GameAudio && Inventory.Money != CashBeforePlot)
                    {
                        GameAudio->PlayMoney();
                    }

                    SyncObjects();
                    FSession Saved{
                        .LevelPath = LevelPath,
                        .LevelName = Level->Name,
                        .Clock = Combat ? Combat->Clock : 0,
                        .Energy = Inventory.Energy,
                        .Shield = Inventory.Shield,
                        .Player = CarriedNow(),
                    };
                    Saved.Player->Object = {};
                    Saved.Objects = CaptureObjects(*Level, Bodies, *Doors, Saved.Player->Inventory.Keys);
                    if (!WriteSavedSession(Settings.Folder, Saved))
                    {
                        std::fprintf(stderr, "Cannot write a session in %s\n", Settings.Folder.string().c_str());
                    }
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
                StepPlacedAnimations(PlacedAnimations, *Objects, *Level, View, int32_t(StepMilliseconds), false);
                if (Combat && !Settings.Details.bLowDetails)
                {
                    StepDecorAnimations(Combat->DecorAnimations, *Objects, View, Combat->Clock, int32_t(StepMilliseconds));
                }

                // The panel's update pass follows the objects' (PanelUpdate at 00435000 with its draw flag clear).
                if (Panel)
                {
                    Panel->Step(PanelValues(), int32_t(StepMilliseconds), Combat ? Combat->MiscRandom : NoMiscRandom);
                    PayForMap();
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

        // Game over (EngineGameOver at 00445a40): the game-over image, then the menu.
        if (DeathTimer && *DeathTimer <= 0)
        {
            // EngineGameOver drops the queued lines and says Game Over.
            ClearVoices();
            if (QueueVoice(EVoice::GameOver, 0, VoiceDuration, 0) && GameAudio)
            {
                GameAudio->UpdateVoices();
            }

            ShowImage(Window, Renderer, Archive, "common\\data\\extra\\game-over.gaf");
            ShowFinalScore();
            return FLevelEnd{ .Kind = ELevelEnd::Menu };
        }

        if (Player)
        {
            SyncObjects();
            // Match the sprite's rounding so fractional movement cannot shift the ship against the camera.
            const int ViewWidth = int(ScreenWidth);
            const int ViewHeight = int(ScreenHeight);
            CameraX = std::clamp(int(SpriteCoordinate(Player->Position.X)) - ViewWidth / 2, 0, std::max(0, int(Level->WorldExtentX) - ViewWidth));
            CameraY = std::clamp(int(SpriteCoordinate(Player->Position.Y)) - ViewHeight / 2, 0, std::max(0, int(Level->WorldExtentY) - ViewHeight));
        }

        CameraX = std::clamp(CameraX, 0.0, std::max(0.0, double(MapWidth - ScreenWidth)));
        CameraY = std::clamp(CameraY, 0.0, std::max(0.0, double(MapHeight - ScreenHeight)));
        // EngineRunLevel moves a trap's or Destroy-around's screen shake (session +0x2200 and +0x2204) one step toward still and flips its side every frame, and adds it to the view after the clamp.
        const auto StepShake = [](int& Offset)
        {
            if (Offset != 0)
            {
                Offset = -(Offset < 0 ? Offset + 1 : Offset - 1);
            }

            return double(Offset);
        };
        CameraX -= StepShake(Inventory.Surprises.ShakeX);
        CameraY -= StepShake(Inventory.Surprises.ShakeY);
        AnimateActors(ActorAnimations, Bodies, *Level, *Objects, ViewCellsAround(int(std::floor(CameraX)), int(std::floor(CameraY)), *Level),
            int32_t(LastStepMilliseconds));
        StepPlacedAnimations(PlacedAnimations, *Objects, *Level, ViewCellsAround(int(std::floor(CameraX)), int(std::floor(CameraY)), *Level), int32_t(LastStepMilliseconds), true);
        UpdateObjectSprites(*Objects, *Level, ActorAnimations, *Doors, PlacedAnimations);
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

        // ShieldAlarmUpdate (0043e370), each frame: with the panel in mode 0 or 1 and the shield at 1% or less, shield-alarm.wav sounds once a second and, with HUD Green Info, "DANGER : SHIELD IS LOW" stays up at 100, 100; the other modes show the bars' own warning.
        if (Player && Panel && !DeathTimer && Panel->GetMode() <= 1)
        {
            constexpr float AlarmRatio = 0.01f;
            constexpr int32_t AlarmPeriodMilliseconds = 1000;
            constexpr int32_t ShieldNoteX = 100;
            constexpr int32_t ShieldNoteY = 100;
            constexpr uint32_t ShieldNoteLifeMilliseconds = 2000;
            constexpr uint32_t PanelScreenFont = 3;
            ShieldAlarmTimer -= int32_t(LastStepMilliseconds);
            if (ShieldAlarmTimer < 1 && Inventory.ShieldMax != 0.f && Inventory.Shield / Inventory.ShieldMax <= AlarmRatio)
            {
                ShieldAlarmTimer = AlarmPeriodMilliseconds;
                if (GameAudio)
                {
                    GameAudio->PlayShieldAlarm();
                }

                if (Settings.Details.bGreenInfo && Messages.Exists(ShieldNoteX, ShieldNoteY))
                {
                    Messages.SetLife(ShieldNoteX, ShieldNoteY, ShieldNoteLifeMilliseconds);
                }
                else if (Settings.Details.bGreenInfo)
                {
                    Messages.Add({
                        .Text = "DANGER : SHIELD IS LOW",
                        .X = float(ShieldNoteX),
                        .Y = float(ShieldNoteY),
                        .LifeMilliseconds = ShieldNoteLifeMilliseconds,
                        .bTypewriter = true,
                        .Gaf = EMessageGaf::Panel,
                        .Font = PanelScreenFont,
                    }, EMessagePlacement::Replace);
                }
            }
        }

        // EngineRunLevel runs the motor (PlayerUpdateMotorSound (00445c50)) while the player lives and no lift is taking it away.
        // The darkness and the extasy surprises play their own tunes (SurpriseStart at 0041c390) and give the previous one back when they end (SurpriseEnd).
        if (GameAudio && Player && PlayerClass)
        {
            if (Inventory.Energy > 0.f && !NextLevel)
            {
                GameAudio->UpdateMotor(*Player, PlayerClass->ThrustMax, Inventory.Surprises.bBlockedMotor, bThrustInput);
            }

            // ObjectsUpdateWeaponSounds (0043df70): object flag 2 is the trigger, which a jammed weapon, a blocked motor and invisibility clear.
            if (Combat && Combat->PlayerSlot >= 1 && Combat->PlayerSlot <= Combat->PlayerWeapons.size())
            {
                if (HeardWeaponSwitches && *HeardWeaponSwitches != Combat->WeaponSwitches)
                {
                    // PlayerSelectWeapon also names the slot: Gun for slot 1 through Mine for slot 9.
                    GameAudio->PlayWeaponSwitch();
                    constexpr size_t NamedSlots = 9;
                    constexpr uint32_t SlotVoiceDelay = 750;
                    constexpr uint32_t SlotVoiceDuration = 500;
                    if (Combat->PlayerSlot >= 1 && Combat->PlayerSlot <= NamedSlots)
                    {
                        QueueVoice(EVoice(size_t(EVoice::Gun) + Combat->PlayerSlot - 1), SlotVoiceDelay, SlotVoiceDuration, 0);
                    }
                }

                HeardWeaponSwitches = Combat->WeaponSwitches;
                const FWeaponClass& Weapon = Combat->Weapons[Combat->PlayerWeapon.Weapon];
                const FAmmoStock* const Stock = PlayerSlotAmmo(*Combat, Inventory);
                const bool bFiring = bFireInput && !DeathTimer && Inventory.Surprises.JammedWeapon != Combat->PlayerWeapon.Weapon &&
                    !Inventory.Surprises.bBlockedMotor && !Player->bInvisible;
                GameAudio->UpdatePlayerWeapon(Weapon, bFiring, Stock != nullptr, Stock ? Stock->Count : 0.f);
            }

            // Every other armed object of the level, its trigger being the shots its weapon took since the last frame.
            if (Combat)
            {
                for (FObjectBody& Body : Bodies)
                {
                    FWeaponState* State = nullptr;
                    const FAmmoStock* Stock = nullptr;
                    if (Body.Turret)
                    {
                        State = &Body.Turret->Weapon;
                        Stock = &Body.Turret->Ammo;
                    }
                    else if (Body.Actor && Body.Actor->Seeker && Body.Actor->Seeker->Weapon)
                    {
                        State = &Body.Actor->Seeker->WeaponState;
                        Stock = &Body.Actor->Seeker->Ammo;
                    }

                    if (!State || Level->Objects[Body.ObjectIndex].bRemoved || State->Weapon >= Combat->Weapons.size())
                    {
                        continue;
                    }

                    const FWeaponClass& Weapon = Combat->Weapons[State->Weapon];
                    GameAudio->UpdateWeapon(Weapon, State->bSoundStarting, State->bSoundFiring, State->bSoundTrigger && !Body.State.bInvisible, true, Stock->Count, 0,
                        Weapon.SoundChannelMask, Weapon.SoundPriority, false);
                    State->bSoundTrigger = false;
                }
            }

            const ESurprise Surprise = Inventory.Surprises.Active;
            const bool bTune = Surprise == ESurprise::Darken || Surprise == ESurprise::Extasy;
            if (Surprise != TuneSurprise)
            {
                if (TuneSurprise != ESurprise::None)
                {
                    GameAudio->RestoreMusic();
                }

                constexpr int DarkenTrack = 2;
                constexpr int ExtasyTrack = 3;
                if (bTune)
                {
                    GameAudio->SwitchMusic(Surprise == ESurprise::Darken ? DarkenTrack : ExtasyTrack);
                }

                TuneSurprise = bTune ? Surprise : ESurprise::None;
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

            if (!Settings.Details.bLowDetails)
            {
                AddDecorSprites(Sprites, Combat->DecorAnimations, *Objects, ViewCellsAround(OriginX, OriginY, *Level));
            }

            std::stable_sort(Sprites.begin(), Sprites.end(), [](const FObjectSprite& A, const FObjectSprite& B) { return SpriteDrawsBefore(A.Depth, B.Depth); });
        }

        // ObjectDraw's fade (object +0x170): an invisible player fades out by one step a frame, down to a quarter of full alpha, and back in once visible again, drawing in mode 3 until the counter is full.
        constexpr float FadeAlphaStep = 0.02f;
        constexpr float MinPlayerAlpha = 0.25f;
        if (Player && Player->bInvisible)
        {
            PlayerFade = std::max(0, PlayerFade - 1);
        }
        else if (PlayerFade < FullFade)
        {
            ++PlayerFade;
        }

        const float PlayerAlpha = std::max(MinPlayerAlpha, float(PlayerFade) * FadeAlphaStep);
        for (const FObjectBody& Body : Bodies)
        {
            int& Fade = ObjectFades[Body.ObjectIndex];
            Fade = Body.bInvisible ? std::max(0, Fade - 1) : Fade < FullFade ? Fade + 1 : Fade;
        }

        // The player's layers from its shadow GAF (AnimInitializeGraphics at 00417b30, kind 0x18), one frame per pose and left out while it fades (ObjectDraw at 00431ae0): the reactor glow under the body and over it while the reactor timer runs, grey as bright as the speed is a share of Thrust.Max, unless Reactor FX is off, and the front light, off while the motor is blocked or with low details.
        if (Player && PlayerFade >= FullFade)
        {
            const auto Shadow = Objects->SheetIndices.find(PlayerObject->ShadowGafPath);
            const auto Body = std::ranges::find_if(Sprites, [&](const FObjectSprite& Sprite) { return Sprite.ObjectIndex == PlayerIndex && !Sprite.bShadow; });
            if (Shadow != Objects->SheetIndices.end() && Body != Sprites.end())
            {
                const std::vector<Gaf::FAnimation>& Animations = Objects->Animations[Shadow->second];
                const FObjectSprite Base = *Body;
                const auto Layer = [&](const size_t Animation, const ESpriteBlend Blend, const uint32_t Diffuse)
                {
                    FObjectSprite Sprite = Base;
                    Sprite.SheetIndex = Shadow->second;
                    const bool bShown = Animation <= Animations.size() && size_t(Player->Pose) <= Animations[Animation - 1].Frames.size();
                    Sprite.Frame = bShown ? Animations[Animation - 1].Frames[size_t(Player->Pose - 1)] : Gaf::FAnimationFrame{};
                    Sprite.Blend = Blend == ESpriteBlend::Opaque ? FrameBlend(Sprite.Frame) : Blend;
                    Sprite.Diffuse = Diffuse;
                    return Sprite;
                };

                constexpr size_t GroundGlowAnimation = 3;
                constexpr size_t ReactorGlowAnimation = 4;
                constexpr size_t FrontLightAnimation = 5;
                constexpr double FullGrey = 255.;
                constexpr uint32_t GreyChannels = 0x010101;
                constexpr uint32_t ChannelMask = 0xff;
                const size_t BodyAt = size_t(Body - Sprites.begin());
                std::vector<FObjectSprite> After;
                const bool bGlow = Settings.Details.bReactorFx && Player->ReactorMilliseconds != 0 && Player->Speed > 0.f && PlayerClass->ThrustMax > 0.f;
                const uint32_t Grey = bGlow ? uint32_t(int64_t(double(Player->Speed) * FullGrey / double(PlayerClass->ThrustMax))) & ChannelMask : 0;
                if (bGlow)
                {
                    After.push_back(Layer(ReactorGlowAnimation, ESpriteBlend::Additive, Grey * GreyChannels));
                }

                if (!Inventory.Surprises.bBlockedMotor && !Settings.Details.bLowDetails)
                {
                    After.push_back(Layer(FrontLightAnimation, ESpriteBlend::Opaque, Base.Diffuse));
                }

                Sprites.insert(Sprites.begin() + std::ptrdiff_t(BodyAt) + 1, After.begin(), After.end());
                if (bGlow)
                {
                    Sprites.insert(Sprites.begin() + std::ptrdiff_t(BodyAt), Layer(GroundGlowAnimation, ESpriteBlend::Additive, Grey * GreyChannels));
                }
            }
        }

        const auto DrawObject = [&](const FObjectSprite& Sprite)
        {
            const Gaf::FAnimationFrame& Frame = Sprite.Frame;
            if (Frame.BitmapIndex == 0 || (Sprite.ObjectIndex != NoObjectIndex && Level->Objects[Sprite.ObjectIndex].bRemoved))
            {
                return;
            }

            // ObjectDraw draws shadows only with the Shadow option, and no part of an Is-detail object with low details.
            const bool bDetailHidden = Settings.Details.bLowDetails && Sprite.ObjectIndex != NoObjectIndex && Level->Objects[Sprite.ObjectIndex].bDetail;
            if ((Sprite.bShadow && !Settings.Details.bShadows) || bDetailHidden)
            {
                return;
            }

            // The hotspot offset is scaled and truncated like the original's __ftol.
            const FSpriteScale Scale = FrameScale(Frame, Sprite.Stretch);
            const float OffsetX = Scale.X != 1.f ? std::trunc(float(Frame.OffsetX) * Scale.X) : float(Frame.OffsetX);
            const float OffsetY = Scale.Y != 1.f ? std::trunc(float(Frame.OffsetY) * Scale.Y) : float(Frame.OffsetY);
            const bool bFadingPlayer = Sprite.ObjectIndex == PlayerIndex && PlayerFade < FullFade;
            const int ObjectFade = Sprite.ObjectIndex < ObjectFades.size() && Sprite.ObjectIndex != PlayerIndex ? ObjectFades[Sprite.ObjectIndex] : FullFade;
            if (ObjectFade == 0)
            {
                return;
            }

            const bool bFadingObject = ObjectFade < FullFade;
            // DrawAnimationFrame at 0045eab0: a frame's event flag 4 sets the diffuse colour to the event value, scaled by the colour the draw passes.
            // The player's light layers fade by pose this way.
            constexpr uint32_t DiffuseEventFlag = 4;
            constexpr uint32_t ColorMask = 0xffffff;
            const uint32_t Diffuse = (Frame.EventFlags & DiffuseEventFlag) != 0 ? ModulateColor(uint32_t(Frame.EventValue) & ColorMask, Sprite.Diffuse) : Sprite.Diffuse;
            Renderer.DrawSprite(Objects->Sheets[Sprite.SheetIndex], Frame.BitmapIndex - 1, Sprite.X + OffsetX - OriginX, Sprite.Y + OffsetY - OriginY,
                Frame.bMirrorX, Frame.bMirrorY, bFadingPlayer || bFadingObject ? ESpriteBlend::Alpha : Sprite.Blend, Scale.X, Scale.Y, UINT32_MAX, Diffuse, 0,
                bFadingPlayer ? PlayerAlpha : bFadingObject ? float(ObjectFade) * FadeAlphaStep : 1.f);
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

        // Darken (SurpriseStart at 0041c390, session +0x20d0 bit 0x20): EngineRunLevel draws the level only in a 376x309 window around the player, black elsewhere, and stretches darkzone.gaf over the window's four quarters, mirrored outward.
        // Then it draws the player and the Is-light objects again over the dark.
        if (Player && Inventory.Surprises.Active == ESurprise::Darken)
        {
            DrawDarkness(*Player, OriginX, OriginY);
            for (const FObjectSprite& Sprite : Sprites)
            {
                if (!Sprite.bShadow && (Sprite.ObjectIndex == PlayerIndex || (Sprite.ObjectIndex < LightObjects.size() && LightObjects[Sprite.ObjectIndex])))
                {
                    DrawObject(Sprite);
                }
            }
        }

        const auto DrawHud = [&](const std::span<const FHudSprite> HudSprites)
        {
            for (const FHudSprite& Hud : HudSprites)
            {
                const FSpriteSheet& Sheet = Hud.Sheet == EHudSheet::Panel    ? PanelSheet
                                            : Hud.Sheet == EHudSheet::VPanel ? VPanelSheet
                                            : Hud.Sheet == EHudSheet::Bonus  ? BonusSheet
                                            : Hud.Sheet == EHudSheet::Icons  ? IconSheet
                                            : Hud.Sheet == EHudSheet::Surprise ? SurpriseSheet
                                                                             : ColorSheet;
                DrawHudSprite(Renderer, Sheet, Hud);
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

        if (Panel && !PageText.empty())
        {
            DrawHud(Panel->DrawShade(int(ScreenWidth), int(ScreenHeight)));
        }

        if (Panel && Player)
        {
            const std::vector<FRadarDot> Radar = FindRadarDots(*Level, Bodies, Player->Position, PlayerIndex, int(ScreenHeight));
            DrawHud(Panel->Draw(PanelValues(), Radar, int(ScreenWidth), int(ScreenHeight), Combat ? Combat->MiscRandom : NoMiscRandom));
            Panel->PostWarnings(PanelValues(), Messages);
            PayForMap();
            // PanelUpdate with HUD Green Info: while locked, one of four warnings by the game clock, held ten seconds; with the bars showing a shield at 1% or less, Shield Low, held fifteen.
            if (Settings.Details.bGreenInfo && !DeathTimer)
            {
                const FPanelValues Values = PanelValues();
                if (Values.bLocked)
                {
                    constexpr std::array LockVoices = { EVoice::Warning, EVoice::WatchOut, EVoice::TakeCare, EVoice::Locked };
                    constexpr uint32_t LockVoiceMask = 3;
                    constexpr uint32_t LockVoiceHold = 10000;
                    QueueVoice(LockVoices[GetVoiceClock() & LockVoiceMask], 0, VoiceDuration, LockVoiceHold);
                }

                constexpr int BarsMode = 2;
                constexpr float ShieldLowRatio = 0.01f;
                constexpr uint32_t ShieldLowHold = 15000;
                if (Panel->GetMode() >= BarsMode && Values.ShieldMax != 0.f && Values.Shield / Values.ShieldMax <= ShieldLowRatio && 0.f < Inventory.Energy)
                {
                    QueueVoice(EVoice::ShieldLow, 0, VoiceDuration, ShieldLowHold);
                }
            }
        }

        if (GameAudio)
        {
            GameAudio->UpdateVoices();
        }

        // PanelDrawBigRadar (004362a0) after the panel: the map, then icons scaled from the level's size, the player's (animation 21) and every lift's (23, lift) and saving plot's (25, saveuk), each list from its last.
        if (bMapShown && MapSheet && Panel && Player)
        {
            constexpr size_t PlayerIcon = 21;
            constexpr size_t LiftIcon = 23;
            constexpr size_t SavingIcon = 25;
            Renderer.DrawSprite(*MapSheet, 0, 0.f, 0.f);
            const double ScaleX = double(ScreenWidth) / double(Level->GridWidth * BlockWidth);
            const double ScaleY = double(ScreenHeight) / double(Level->GridHeight * BlockHeight);
            const auto Icon = [&](const size_t Animation, const float X, const float Y)
            {
                if (const std::optional<FHudSprite> Sprite = Panel->DrawAnimation(Animation, float(TruncateLow(double(X) * ScaleX)), float(TruncateLow(double(Y) * ScaleY))))
                {
                    DrawHud(std::span(&*Sprite, 1));
                }
            };
            Icon(PlayerIcon, Player->Position.X, Player->Position.Y);
            for (const FLevelObject& Object : Level->Objects | std::views::reverse)
            {
                if (Object.SubType == "lift")
                {
                    Icon(LiftIcon, Object.X, Object.Y);
                }
            }

            for (const FSavingPlot& Area : SavingPlots | std::views::reverse)
            {
                if (Area.Plot)
                {
                    Icon(SavingIcon, Level->Objects[*Area.Plot].X, Level->Objects[*Area.Plot].Y);
                }
            }
        }

        if (Panel && !PageText.empty())
        {
            constexpr size_t PageFont = 3;
            DrawHud(Panel->DrawTypedPage(PageFont, PageText, PageTyped, int(ScreenWidth), int(ScreenHeight)));
        }

        if (DeathTimer && FailedGaf && FailedGaf->Animations.size() >= 2)
        {
            if (!FailedSheet)
            {
                FailedSheet = Renderer.CreateSpriteSheet(FailedGaf->Bitmaps);
            }

            const float CentreX = float(int32_t(ScreenWidth) >> 1);
            const float CentreY = float(int32_t(ScreenHeight) >> 1);
            for (const size_t Animation : { size_t(1), size_t(0) })
            {
                if (!FailedGaf->Animations[Animation].Frames.empty())
                {
                    DrawHudSprite(Renderer, *FailedSheet, { .Frame = FailedGaf->Animations[Animation].Frames.front(), .X = CentreX, .Y = CentreY });
                }
            }
        }

        if (Sight && Player)
        {
            if (!SightSheet)
            {
                SightSheet = Renderer.CreateSpriteSheet(SightGaf->Bitmaps);
            }

            UpdateAnimation(SightPlayback, *Sight, int32_t(LastStepMilliseconds));
            const uint32_t Frame = SightPlayback.bEnded ? 0 : SightPlayback.Frame;
            if (Frame >= 1 && Frame <= Sight->Frames.size())
            {
                const std::pair<int32_t, int32_t> Mouse = ScreenCursor(Window);
                DrawHudSprite(Renderer, *SightSheet, { .Frame = Sight->Frames[Frame - 1], .X = float(Mouse.first), .Y = float(Mouse.second) });
            }
        }

        if (bPaused && PauseGaf && !PauseGaf->Animations.empty() && !PauseGaf->Animations.front().Frames.empty())
        {
            constexpr uint32_t PauseDrawMode = 7;
            if (!PauseSheet)
            {
                PauseSheet = Renderer.CreateSpriteSheet(PauseGaf->Bitmaps);
            }

            Gaf::FAnimationFrame Frame = PauseGaf->Animations.front().Frames.front();
            Frame.DrawFlags = PauseDrawMode;
            if (Frame.BitmapIndex != 0 && Frame.BitmapIndex <= PauseGaf->Bitmaps.size())
            {
                const Gaf::FFrame& Bitmap = PauseGaf->Bitmaps[Frame.BitmapIndex - 1];
                const float X = float((int32_t(ScreenWidth) - int32_t(Bitmap.Width)) / 2 - Bitmap.TrimX);
                const float Y = float((int32_t(ScreenHeight) - int32_t(Bitmap.Height)) / 2 - Bitmap.TrimY);
                Renderer.DrawSprite(*PauseSheet, Frame.BitmapIndex - 1, X, Y, false, false, FrameBlend(Frame));
            }
        }

        // EngineRunLevel with Gamma FX, each frame unless a lift is taking the player: the gamma ramp takes the offsets (FUN_0045ffc0), a positive one lifting a channel toward white by offset / 256 of the way and a negative one scaling it by (offset + 255) / 256.
        // While the player lives each offset then steps one toward zero; the dead player's red stays.
        if (MenuFadeStep < MenuFadeSteps)
        {
            // Each step darkens by a quarter less: the gamma offset (step - 4) * 255 / 4, or without Gamma FX the rows at or past the step in every four turned black.
            constexpr int FadeQuarters = 4;
            if (Settings.Details.bGammaFx)
            {
                Inventory.Surprises.Gamma.fill((MenuFadeStep - FadeQuarters) * 0xff / FadeQuarters);
            }
            else
            {
                if (!WhiteSheet)
                {
                    constexpr uint16_t White = 0x7fff;
                    WhiteSheet = Renderer.CreateSpriteSheet({ Gaf::FFrame{ .Width = 1, .Height = 1, .Pixels = { White } } });
                }

                constexpr uint32_t Black = 0;
                for (int Row = 0; Row < int(ScreenHeight); ++Row)
                {
                    if (Row % FadeQuarters >= MenuFadeStep)
                    {
                        Renderer.DrawSprite(*WhiteSheet, 0, 0.f, float(Row), false, false, ESpriteBlend::Opaque, float(ScreenWidth), 1.f, UINT32_MAX, Black);
                    }
                }
            }

            // FUN_00469780 waits 50 ms between steps with Gamma FX and 25 without.
            constexpr uint32_t GammaFadeStepMilliseconds = 50;
            constexpr uint32_t LineFadeStepMilliseconds = 25;
            const uint32_t FadeStepMilliseconds = Settings.Details.bGammaFx ? GammaFadeStepMilliseconds : LineFadeStepMilliseconds;
            if (ClockMilliseconds() - MenuFadeTime >= FadeStepMilliseconds)
            {
                MenuFadeTime = ClockMilliseconds();
                ++MenuFadeStep;
            }
        }

        // During the finale the gamma ramp goes white for each flash and stays at the base otherwise; for the last 2.5 seconds FUN_00446060 fades it from white to black along ln(e - t (e - 1) / 2500).
        if (Settings.Details.bGammaFx && FinaleTimer > 0)
        {
            std::array<int32_t, 3>& Gamma = Inventory.Surprises.Gamma;
            if (FinaleTimer >= FinaleFadeMilliseconds)
            {
                constexpr int32_t White = 0xff;
                Gamma.fill(bFinaleFlash ? White : 0);
                bFinaleFlash = false;
            }
            else
            {
                constexpr double E = 2.718281828459045;
                constexpr double FadeRate = 0.0004;
                constexpr double FadeSpan = 0x1fe;
                constexpr int32_t FadeOffset = 0xff;
                const double Elapsed = double(FinaleFadeMilliseconds - FinaleTimer);
                Gamma.fill(TruncateLow(FadeSpan * std::log(E - Elapsed * (E - 1.) * FadeRate)) - FadeOffset);
            }
        }

        if (Settings.Details.bGammaFx && !NextLevel)
        {
            std::array<int32_t, 3>& Gamma = Inventory.Surprises.Gamma;
            constexpr int32_t GammaLimit = 0xff;
            const std::array<int32_t, 3> Shown = { std::clamp(Gamma[0] + Settings.Gamma[0], -GammaLimit, GammaLimit),
                std::clamp(Gamma[1] + Settings.Gamma[1], -GammaLimit, GammaLimit), std::clamp(Gamma[2] + Settings.Gamma[2], -GammaLimit, GammaLimit) };
            if (!DeathTimer)
            {
                for (int32_t& Channel : Gamma)
                {
                    Channel -= Channel > 0 ? 1 : Channel < 0 ? -1 : 0;
                }
            }

            const auto Pack = [&](const auto Part) { return uint32_t(Part(Shown[0])) << 16 | uint32_t(Part(Shown[1])) << 8 | uint32_t(Part(Shown[2])); };
            const uint32_t Lift = Pack([](const int32_t Offset) { return std::max(Offset, 0); });
            const uint32_t Darken = Pack([](const int32_t Offset) { return Offset < 0 ? 1 - Offset : 0; });
            if (Lift != 0 || Darken != 0)
            {
                if (!WhiteSheet)
                {
                    constexpr uint16_t White = 0x7fff;
                    WhiteSheet = Renderer.CreateSpriteSheet({ Gaf::FFrame{ .Width = 1, .Height = 1, .Pixels = { White } } });
                }

                const float Width = float(ScreenWidth);
                const float Height = float(ScreenHeight);
                if (Lift != 0)
                {
                    Renderer.DrawSprite(*WhiteSheet, 0, 0.f, 0.f, false, false, ESpriteBlend::Screen, Width, Height, UINT32_MAX, Lift);
                }

                if (Darken != 0)
                {
                    Renderer.DrawSprite(*WhiteSheet, 0, 0.f, 0.f, false, false, ESpriteBlend::InverseSourceColor, Width, Height, UINT32_MAX, Darken);
                }
            }
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

        // EngineRunLevel ends the game after a starred lift: the registered game's two ending pages over theend.tga (FUN_00402890), then the score table and the menu.
        if (NextLevel && NextLevel->starts_with('*'))
        {
            if (bShowNetworkFailure && Panel)
            {
                ShowNetworkFailure(Window, Renderer, *Panel);
            }

            if (GameAudio)
            {
                GameAudio->PlayFade();
            }

            constexpr int32_t FirstPageLeft = 0x46;
            constexpr int32_t SecondPageLeft = 0x8c;
            if (Panel)
            {
                ShowTextPage(Window, Renderer, Archive, *Panel, "common\\data\\extra\\theend.tga", "common\\data\\extra\\theend-en.txt", FirstPageLeft);
                ShowTextPage(Window, Renderer, Archive, *Panel, "common\\data\\extra\\theend.tga", "common\\data\\extra\\theend-en-2.txt", SecondPageLeft);
            }

            ShowFinalScore();

            return FLevelEnd{ .Kind = ELevelEnd::Menu };
        }

        // The level ends after the frame in which the player took a lift.
        if (NextLevel && Player && PlayerObject != Level->Objects.end())
        {
            Carried = CarriedNow();
            return FLevelEnd{ .Kind = ELevelEnd::Lift, .NextLevel = *NextLevel };
        }
    }

    return FLevelEnd{};
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
    bool bVerifyIntro = false;
    // Development aid: --level and --start skip the main menu at startup.
    bool bSkipMenu = false;
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
        else if (std::strcmp(Args[Index], "--verify-intro") == 0)
        {
            bVerifyIntro = true;
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
            bSkipMenu = true;
        }
        else if (float X = 0.f, Y = 0.f; std::strcmp(Args[Index], "--start") == 0 && Index + 1 < ArgCount && std::sscanf(Args[Index + 1], "%f,%f", &X, &Y) == 2)
        {
            Start = { X, Y };
            bSkipMenu = true;
            ++Index;
        }
        else
        {
            DataPath = Args[Index];
        }
    }

    const bool bDataPathGiven = DataPath.has_value();
    bool bRememberDataPath = bDataPathGiven;
    const std::filesystem::path GameFolderFile = UserDataDirectory() / "game-folder.txt";
    if (!DataPath)
    {
        const auto Directory = ExecutableDirectory();
        if (!Directory)
        {
            std::fprintf(stderr, "%s\n", Directory.error().c_str());
            return 1;
        }

        DataPath = *Directory / "vz-data.rsc";
        // An installed binary (such as /usr/bin from the Arch package) has no data beside it, so it uses the game folder last given on the command line or picked.
        if (std::error_code Error; !std::filesystem::exists(*DataPath, Error))
        {
            std::string Remembered;
            if (std::ifstream File(GameFolderFile); std::getline(File, Remembered) && !Remembered.empty())
            {
                DataPath = std::filesystem::path(std::u8string(Remembered.begin(), Remembered.end()));
            }
        }
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
    // Without a path on the command line, the player picks the game folder until it holds the archive or they cancel.
    while (!Archive.Open(*DataPath))
    {
        const std::optional<std::filesystem::path> Picked = bDataPathGiven ? std::nullopt : PickFolder("Select the Vyruz folder (the one holding vz-data.rsc)");
        if (!Picked)
        {
            std::fprintf(stderr, "Cannot open %s. Pass the game folder holding vz-data.rsc once; later runs remember it.\n", DataPath->string().c_str());
            return 1;
        }

        DataPath = *Picked / "vz-data.rsc";
        bRememberDataPath = true;
    }

    if (bRememberDataPath)
    {
        std::error_code Error;
        std::filesystem::create_directories(GameFolderFile.parent_path(), Error);
        const std::u8string Absolute = std::filesystem::absolute(*DataPath, Error).u8string();
        std::ofstream(GameFolderFile) << std::string(Absolute.begin(), Absolute.end()) << '\n';
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
        // The slide show's archive, when it is there, takes the same Targa loader.
        size_t SlidesFailed = 0;
        const size_t SlidesDecoded = LoadSlides(DataPath->parent_path(), &SlidesFailed).size();
        std::printf("Slides: %zu decoded, %zu failed\n", SlidesDecoded, SlidesFailed);
        const int GafResult = VerifyGafs(Archive);
        return SlidesFailed != 0 ? 1 : GafResult;
    }
    if (bVerifyIntro)
    {
        return VerifyIntro(DataPath->parent_path());
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
        const std::expected<std::string, std::string> SessionCheck = VerifySessions(Archive);
        std::printf("Sessions: %s\n", SessionCheck ? SessionCheck->c_str() : SessionCheck.error().c_str());
        const bool bScores = VerifyScores();
        std::printf("Scores: %s\n", bScores ? "passed" : "failed");
        std::printf("Frame loop: %s, terrain collision: %s, player movement: %s\n", bFrameLoop ? "passed" : "failed",
            Collision ? "passed" : Collision.error().c_str(), Movement ? "passed" : Movement.error().c_str());
        return bFrameLoop && Collision && Movement && Paths && Demo && Combat && Patrols && Doors && SessionCheck && bScores ? 0 : 1;
    }

    if (!glfwInit())
    {
        std::fprintf(stderr, "glfwInit failed\n");
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    // Wayland compositors and X11 window managers find the icon through the desktop entry named after this id (Resources/Linux/openvyruz.desktop).
    constexpr const char* AppId = "openvyruz";
    glfwWindowHintString(GLFW_WAYLAND_APP_ID, AppId);
    glfwWindowHintString(GLFW_X11_CLASS_NAME, AppId);
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
    std::expected<FMenuArt, std::string> MenuArt = FMenuArt::Load(Archive);
    if (!MenuArt)
    {
        std::fprintf(stderr, "No menus: %s\n", MenuArt.error().c_str());
    }

    // ConfigLoad (0042f570) reads vzconf.txt from the game folder, else the archive's VZCONF.TXT: session-name names the session folder and the player, use-keyboard, use-joypad and use-mouse choose the input device, the last set one winning, and the detail keywords set the Video Details options.
    // A flag is set when its value reads as a non-zero number (atoi).
    FSettings Settings{ .PlayerName = "Player" };
    {
        struct FDetailKey
        {
            std::string_view Name;
            bool FVideoDetails::* Field = nullptr;
            bool bInverted = false;
        };
        constexpr std::array DetailKeys{
            FDetailKey{ .Name = "shadow", .Field = &FVideoDetails::bShadows },
            FDetailKey{ .Name = "reactor-fx", .Field = &FVideoDetails::bReactorFx },
            FDetailKey{ .Name = "hud-green-info", .Field = &FVideoDetails::bGreenInfo },
            FDetailKey{ .Name = "translucent-panel", .Field = &FVideoDetails::bTranslucentPanel },
            FDetailKey{ .Name = "tv-borders", .Field = &FVideoDetails::bTvBorders },
            FDetailKey{ .Name = "gamma-fx", .Field = &FVideoDetails::bGammaFx },
            FDetailKey{ .Name = "low-details", .Field = &FVideoDetails::bLowDetails },
            FDetailKey{ .Name = "high-details", .Field = &FVideoDetails::bLowDetails, .bInverted = true },
        };
        struct FAudioSwitch
        {
            std::string_view Name;
            bool FAudioSettings::* Field = nullptr;
        };
        constexpr std::array AudioSwitches{
            FAudioSwitch{ .Name = "music", .Field = &FAudioSettings::bMusic },
            FAudioSwitch{ .Name = "sound-fx", .Field = &FAudioSettings::bSoundFx },
            FAudioSwitch{ .Name = "voices", .Field = &FAudioSettings::bVoices },
        };
        struct FAudioVolume
        {
            std::string_view Name;
            int FAudioSettings::* Field = nullptr;
        };
        constexpr std::array AudioVolumes{
            FAudioVolume{ .Name = "music-volume", .Field = &FAudioSettings::MusicVolume },
            FAudioVolume{ .Name = "sound-fx-volume", .Field = &FAudioSettings::SoundFxVolume },
            FAudioVolume{ .Name = "voices-volume", .Field = &FAudioSettings::VoicesVolume },
        };
        const auto IsSet = [](const std::string_view Value)
        {
            int Number = 0;
            std::from_chars(Value.data(), Value.data() + Value.size(), Number);
            return Number != 0;
        };

        const std::optional<std::vector<uint8_t>> DifficultyFile = Archive.Read("common\\difficulty.3dc");
        const FDifficultyTable Difficulties = LoadDifficulties(DifficultyFile ? std::string_view(reinterpret_cast<const char*>(DifficultyFile->data()), DifficultyFile->size()) : std::string_view());
        size_t DifficultyLevel = Difficulties.DefaultLevel;
        std::string Text(reinterpret_cast<const char*>(Config->data()), Config->size());
        std::error_code Error;
        for (const std::filesystem::directory_entry& Entry : std::filesystem::directory_iterator(DataPath->parent_path(), Error))
        {
            std::string Name = Entry.path().filename().string();
            std::ranges::transform(Name, Name.begin(), [](const unsigned char Character) { return char(std::tolower(Character)); });
            constexpr std::uintmax_t MaxConfigBytes = 65536;
            if (Name == "vzconf.txt" && Entry.file_size(Error) <= MaxConfigBytes)
            {
                std::ifstream File(Entry.path(), std::ios::binary);
                Text.assign(std::istreambuf_iterator<char>(File), std::istreambuf_iterator<char>());
            }
        }

        for (const auto Part : std::views::split(std::string_view(Text), '\n'))
        {
            const std::string_view Row(Part.begin(), Part.end());
            const size_t Equals = Row.find('=');
            if (Row.starts_with('#') || Equals == std::string_view::npos)
            {
                continue;
            }

            constexpr std::string_view Blanks = " \t\r";
            const auto Trim = [&](std::string_view Value)
            {
                Value.remove_prefix(std::min(Value.size(), Value.find_first_not_of(Blanks)));
                return Value.substr(0, Value.find_last_not_of(Blanks) + 1);
            };
            const std::string_view Key = Trim(Row.substr(0, Equals));
            const std::string_view Value = Trim(Row.substr(Equals + 1));
            if (Key == "session-name" && !Value.empty() && Value.find_first_of("\\/:*?\"<>|.") == std::string_view::npos)
            {
                Settings.PlayerName = std::string(Value);
            }
            else if (int Level = 0; Key == "difficulty-level" && std::from_chars(Value.data(), Value.data() + Value.size(), Level).ec == std::errc{} && Level >= 0 &&
                Level < int(DifficultyLevelCount))
            {
                DifficultyLevel = size_t(Level);
            }
            else if (Key == "gamma")
            {
                size_t Channel = 0;
                for (const auto Field : std::views::split(Value, ','))
                {
                    const std::string_view Number = Trim(std::string_view(Field.begin(), Field.end()));
                    if (Channel < Settings.Gamma.size())
                    {
                        std::from_chars(Number.data(), Number.data() + Number.size(), Settings.Gamma[Channel++]);
                    }
                }
            }
            else if ((Key == "use-mouse" || Key == "use-keyboard" || Key == "use-joypad") && IsSet(Value))
            {
                Settings.bMouse = Key == "use-mouse";
            }
            else if (const auto Detail = std::ranges::find(DetailKeys, Key, &FDetailKey::Name); Detail != DetailKeys.end())
            {
                Settings.Details.*Detail->Field = IsSet(Value) != Detail->bInverted;
            }
            else if (const auto Switch = std::ranges::find(AudioSwitches, Key, &FAudioSwitch::Name); Switch != AudioSwitches.end())
            {
                Settings.Audio.*Switch->Field = IsSet(Value);
            }
            else if (const auto Volume = std::ranges::find(AudioVolumes, Key, &FAudioVolume::Name); Volume != AudioVolumes.end())
            {
                // ConfigLoad clamps the volumes to 0..0xffff.
                int Number = 0;
                std::from_chars(Value.data(), Value.data() + Value.size(), Number);
                Settings.Audio.*Volume->Field = std::clamp(Number, 0, Sound::MaxVolume);
            }
        }

        Settings.Difficulty = Difficulties.Levels[DifficultyLevel];
    }

    if (Panel)
    {
        Panel->SetDetails(Settings.Details);
    }

    // VzRun at 00446e20 opens the sound (VzOpenSound (004477e0)) and applies the volumes; the music waits for the story page and the intro.
    FAudio Audio(Archive);
    if (Audio.Open())
    {
        GameAudio = &Audio;
        Audio.Apply(Settings.Audio);
    }

    Settings.Folder = SessionDirectory(Settings.PlayerName);
    std::optional<FCarriedPlayer> Carried;
    std::optional<FSession> Session = ReadSessionFile(DefaultSessionPath(Settings.Folder));
    std::optional<FSession> Restore;
    std::optional<std::vector<FDemoRecord>> Demo;
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

    // VzRun's loop (VzMenuLoop at 00447820): the main menu, then levels until a menu or the game's end brings it back. New Game starts at LevelPath.
    int Result = 0;
    bool bMenu = MenuArt && !bSkipMenu;
    // VzRun shows the story page (ShowStoryPage at 00447c80, read aloud, for up to 30 seconds), then plays the intro, before the main menu.
    if (bMenu && Panel)
    {
        constexpr int32_t StoryLeft = 0x80;
        constexpr uint32_t StoryMilliseconds = 30000;
        if (GameAudio)
        {
            GameAudio->PlayStoryVoice();
        }

        ShowTextPage(Window, Renderer, Archive, *Panel, "common\\data\\extra\\story.tga", "common\\data\\extra\\story-en.txt", StoryLeft, StoryMilliseconds);
        if (GameAudio)
        {
            GameAudio->StopStoryVoice();
        }

        Renderer.ReleaseSpriteSheets();
    }

    if (bMenu && FindIntro(DataPath->parent_path()))
    {
        PlayIntro(Window, Renderer, DataPath->parent_path());
    }

    // Then MusicLoad and MusicStart, before VzMenuLoop.
    if (GameAudio)
    {
        GameAudio->StartMusic();
    }

    for (std::string Path = LevelPath;;)
    {
        if (bMenu)
        {
            FSession Chosen;
            const EMenuChoice Choice = RunMainMenu(Window, Renderer, Archive, *MenuArt, Session ? &*Session : nullptr, Settings.Folder, DataPath->parent_path(), Chosen);
            Renderer.ReleaseSpriteSheets();
            if (Choice == EMenuChoice::Quit)
            {
                break;
            }

            // VzMenuLoop's demo: CAVE\DEMO.3DR on the CAVE\DEMO level, then the menu again.
            if (Choice == EMenuChoice::Demo)
            {
                std::expected<std::vector<FDemoRecord>, std::string> Records = LoadDemo(Archive);
                if (!Records)
                {
                    std::fprintf(stderr, "Cannot load recorded demo: %s\n", Records.error().c_str());
                    continue;
                }

                Demo = std::move(*Records);
                Path = "cave\\demo";
                Carried.reset();
                Restore.reset();
                bMenu = false;
            }
            else
            {
                Path = Choice == EMenuChoice::NewGame ? LevelPath : Chosen.LevelPath;
                Carried = Choice == EMenuChoice::NewGame ? std::nullopt : Chosen.Player;
                Restore = Choice == EMenuChoice::NewGame ? std::nullopt : std::optional(std::move(Chosen));
                bMenu = false;
            }
        }

        const std::expected<FLevelEnd, std::string> End = RunLevel(Window, Renderer, Archive, Path, Start, bAutoFire, Carried, DebugLevels, Panel ? &*Panel : nullptr,
            MenuArt ? &*MenuArt : nullptr, Session, Restore ? &*Restore : nullptr, Settings, Demo ? &*Demo : nullptr);
        Restore.reset();
        Renderer.ReleaseSpriteSheets();
        // EngineRunLevel stops the player's weapon and motor, and LoopLevels brings the first tune back.
        if (GameAudio)
        {
            GameAudio->StopLevelChannels();
            if (GameAudio->GetMusicTrack() > 1)
            {
                GameAudio->SwitchMusic(1);
            }
        }

        // --start places the player in the first level only.
        Start.reset();
        // A demo always comes back to the menu.
        if (Demo && End && End->Kind != ELevelEnd::Quit)
        {
            Demo.reset();
            bMenu = MenuArt.has_value();
            Path = LevelPath;
            Carried.reset();
            continue;
        }

        Demo.reset();
        if (!End)
        {
            std::fprintf(stderr, "%s\n", End.error().c_str());
            Result = 1;
            break;
        }

        if (End->Kind == ELevelEnd::Quit)
        {
            break;
        }

        if (End->Kind == ELevelEnd::Lift)
        {
            Path = End->NextLevel;
        }
        else if (End->Kind == ELevelEnd::Restart)
        {
            // SessionLoad mode 2 loads the default session.
            Path = Session->LevelPath;
            Carried = Session->Player;
            Restore = Session;
        }
        else
        {
            bMenu = MenuArt.has_value();
            Path = LevelPath;
            Carried.reset();
        }
    }

    Renderer.Shutdown();
    glfwDestroyWindow(Window);
    glfwTerminate();
    return Result;
}
