#include "Game/Audio.h"
#include "Game/Collision.h"
#include "Game/Engine.h"
#include "Game/Input.h"
#include "Game/Language.h"
#include "Game/Level.h"
#include "Game/Menu.h"
#include "Game/Misc.h"
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
#include "Tools/Compress.h"
#include "Tools/Extract.h"

#include <GLFW/glfw3.h>
#include <GamepadMappings.h>

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
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
// The game's sound, which the menus and the level loop reach as the original reaches its globals; null while it has no output.
FAudio* GameAudio = nullptr;
// The game folder, where the original keeps vz.sco, vzconf.txt and the session folders.
std::filesystem::path GameFolder;

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
constexpr std::string_view GeneratorKind = "staticgen";

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

// The 1-based frame UpdateAnimation (0045e7e0) shows Age milliseconds after StartAnimation, without the frames' sounds; an animation that does not loop holds its last frame.
uint32_t AnimationFrameAt(const Gaf::FAnimation& Animation, const uint32_t Age)
{
    constexpr uint32_t LoopFlag = 2;
    const uint32_t Count = uint32_t(Animation.Frames.size());
    const bool bLoop = (Animation.Flags & LoopFlag) != 0;
    const uint32_t LoopStart = Animation.LoopStart >= 1 && Animation.LoopStart <= Count ? Animation.LoopStart : 1;
    int64_t Left = Age;
    uint32_t Frame = 1;
    // A pass over every frame without any time spent stops on the frame reached.
    for (uint32_t Steps = 0; Count != 0 && Steps <= Count; ++Steps)
    {
        const int64_t Duration = int64_t(Animation.Frames[Frame - 1].Duration) * AnimationPeriodMilliseconds;
        if (Left < Duration)
        {
            return Frame;
        }

        Left -= Duration;
        Steps = Duration > 0 ? 0 : Steps;
        if (++Frame > Count)
        {
            if (!bLoop)
            {
                return Count;
            }

            Frame = LoopStart;
        }
    }

    return Count == 0 ? 0 : Frame;
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
            Level->Objects.size(), Terrain->GetShapes().size(), Level->MissingTileRefs, Level->PatrolPaths.size(), Patrollers, Unrouted, Level->Name.c_str());        ++Loaded;
    }
    std::printf("Levels: %d loaded, %d failed\n", Loaded, Failed);
    return Failed == 0 && Loaded > 0 ? 0 : 1;
}

// Sprites of runtime FX objects belong to no placed object.
constexpr size_t NoObjectIndex = SIZE_MAX;

struct FObjectSprite
{
    // Added to the bitmap as 0xRRGGBB.
    uint32_t Specular = 0;
    size_t SheetIndex = 0;
    size_t ObjectIndex = 0;
    bool bShadow = false;
    // A tank's turret top: the animation after its 24 body poses, one frame per top pose (ObjectDraw at 00431ae0, kind 0x20).
    bool bTop = false;
    // A Use-delta-packing lift or gate stores later frames as changes over the first, which ObjectDraw (00431ae0, FUN_00433710) draws underneath first.
    bool bDeltaBase = false;
    // A turret's fixed base: the animation after its 24 top poses, drawn under the top (ObjectDraw, kind 0x1e).
    bool bTurretBase = false;
    // A generator's working light, its Shadow-gaf's animation 1: played while it is closed for a birth and drawn above its body (ObjectDraw, kind 0x17).
    bool bGeneratorLight = false;
    Gaf::FAnimationFrame Frame;
    float X = 0.f;
    float Y = 0.f;
    int64_t Depth = 0;
    ESpriteBlend Blend = ESpriteBlend::Opaque;
    float Stretch = 1.f;
    uint32_t Diffuse = 0xffffff;
    // Below one, drawn blended by this alpha.
    float Alpha = 1.f;
    // Degrees clockwise about the sprite's position (DrawAnimationFrame's turn).
    float Rotation = 0.f;
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
    // A generator's light, which an open generator hides and rewinds to its first frame.
    bool bGeneratorLight = false;
    bool bHidden = false;
    FAnimationPlayback Playback;
};

std::optional<FPlacedAnimation> PlacedAnimationFor(const FObjectSprite& Sprite, const FObjectSprites& Sprites, const FLevel& Level,
    const std::span<const std::optional<FActorAnimation>> Actors)
{
    constexpr uint32_t LoopFlag = 2;
    const FLevelObject& Object = Level.Objects[Sprite.ObjectIndex];
    // A generator's light plays from its first animation whatever its length; its body holds its first frame.
    if (Sprite.bGeneratorLight)
    {
        const Gaf::FAnimation& Light = Sprites.Animations[Sprite.SheetIndex].front();
        return FPlacedAnimation{ .ObjectIndex = Sprite.ObjectIndex, .SheetIndex = Sprite.SheetIndex, .bGeneratorLight = true, .Playback = StartAnimation(Light) };
    }

    if (Sprite.bShadow || Sprite.bTop || Sprite.bTurretBase || Sprite.bDeltaBase || Actors[Sprite.ObjectIndex] || Object.SubType == "lift" || Object.SubType == "gate" ||
        Object.SubType == TurretKind || Object.SubType == "vyruz" || Object.SubType == GeneratorKind)
    {
        return std::nullopt;
    }

    const size_t Index = ObjectAnimationIndex(Object, false);
    const Gaf::FAnimation& Animation = Sprites.Animations[Sprite.SheetIndex][Index];
    if (Animation.Frames.size() <= 1 || (Animation.Flags & LoopFlag) == 0)
    {
        return std::nullopt;
    }

    return FPlacedAnimation{
        .ObjectIndex = Sprite.ObjectIndex,
        .SheetIndex = Sprite.SheetIndex,
        .Animation = Index,
        .bGraphicUpdate = ParseLevelFloat(Object.Properties.Find("Graphic-update-needed")).value_or(0.f) != 0.f,
        .Playback = StartAnimation(Animation),
    };
}

std::vector<FPlacedAnimation> CreatePlacedAnimations(const FObjectSprites& Sprites, const FLevel& Level, const std::span<const std::optional<FActorAnimation>> Actors)
{
    std::vector<FPlacedAnimation> Result;
    for (const FObjectSprite& Sprite : Sprites.Sprites)
    {
        if (std::optional<FPlacedAnimation> Placed = PlacedAnimationFor(Sprite, Sprites, Level, Actors))
        {
            Result.push_back(std::move(*Placed));
        }
    }

    return Result;
}

// ObjectDraw for a generator (kind 0x17): while it stands open (passable, object +0x5c bit 1) its light is hidden and set back to its first frame.
void SyncGeneratorLights(std::vector<FPlacedAnimation>& Animations, const FObjectSprites& Sprites, const std::span<const FObjectBody> Bodies)
{
    for (FPlacedAnimation& Placed : Animations)
    {
        const auto Body = Placed.bGeneratorLight ? std::ranges::find(Bodies, Placed.ObjectIndex, &FObjectBody::ObjectIndex) : Bodies.end();
        if (Body == Bodies.end())
        {
            continue;
        }

        Placed.bHidden = Body->bPassable;
        if (Placed.bHidden)
        {
            Placed.Playback = StartAnimation(Sprites.Animations[Placed.SheetIndex].front());
        }
    }
}

void StepPlacedAnimations(std::vector<FPlacedAnimation>& Animations, const FObjectSprites& Sprites, const FLevel& Level, const FViewCells View, const int32_t StepMilliseconds,
    const bool bDrawPass)
{
    for (FPlacedAnimation& Placed : Animations)
    {
        const FLevelObject& Object = Level.Objects[Placed.ObjectIndex];
        const bool bSteps = Placed.bGraphicUpdate ? !bDrawPass : View.Contains({ .X = Object.X, .Y = Object.Y });
        if (bSteps && !Object.bRemoved && !Placed.bHidden)
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

// EngineVisitViewObjects at 00433b30 visits from one column left of the view and two rows above it, to three columns and five rows past its size (13 by 20 at 640x480), clamped to the grid.
FViewCells ViewCellsAround(const int CameraX, const int CameraY, const int ViewWidth, const FLevel& Level)
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
        .Right = std::min(Left + ViewWidth / int(BlockWidth) + ExtraColumns, int(Level.GridWidth)),
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

struct FMapBox
{
    // Where the map lies in the view.
    float Left = 0.f;
    float Top = 0.f;
    float Width = 0.f;
    float Height = 0.f;
    // The part of the level it shows, in world pixels.
    float WorldLeft = 0.f;
    float WorldTop = 0.f;
    float WorldWidth = 0.f;
    float WorldHeight = 0.f;
};

// The port's map shows the cells that hold blocks, in the level's shape, as large as fits the view inside a margin and centred, where the original stretches the whole grid over the screen.
FMapBox LevelMapBox(const FLevel& Level, const float ViewWidth)
{
    const size_t CellCount = size_t(Level.GridWidth) * Level.GridHeight;
    uint32_t Left = Level.GridWidth;
    uint32_t Top = Level.GridHeight;
    uint32_t Right = 0;
    uint32_t Bottom = 0;
    for (size_t Cell = 0; Cell < CellCount; ++Cell)
    {
        for (uint32_t Layer = 0; Layer < Level.LayerCount; ++Layer)
        {
            const size_t Index = size_t(Layer) * CellCount + Cell;
            if (Index < Level.Map.size() && Level.Map[Index] != 0 && Level.Map[Index] <= Level.Blocks.size())
            {
                const uint32_t X = uint32_t(Cell % Level.GridWidth);
                const uint32_t Y = uint32_t(Cell / Level.GridWidth);
                Left = std::min(Left, X);
                Top = std::min(Top, Y);
                Right = std::max(Right, X + 1);
                Bottom = std::max(Bottom, Y + 1);
                break;
            }
        }
    }

    if (Right <= Left || Bottom <= Top)
    {
        return {};
    }

    constexpr float Margin = 40.f;
    FMapBox Box{
        .WorldLeft = float(Left * BlockWidth),
        .WorldTop = float(Top * BlockHeight),
        .WorldWidth = float((Right - Left) * BlockWidth),
        .WorldHeight = float((Bottom - Top) * BlockHeight),
    };
    const float Scale = std::min((ViewWidth - 2.f * Margin) / Box.WorldWidth, (ScreenHeight - 2.f * Margin) / Box.WorldHeight);
    Box.Width = std::floor(Box.WorldWidth * Scale);
    Box.Height = std::floor(Box.WorldHeight * Scale);
    Box.Left = std::floor((ViewWidth - Box.Width) / 2.f);
    Box.Top = std::floor((ScreenHeight - Box.Height) / 2.f);
    return Box;
}

// The map's picture (MapToggle at 00436460 with GobRenderMap at 0046b120): every MAP_ layer from the first, each block stretched into its cell's share of a screen-sized bitmap, which starts black; pixels left at 0x800 are transparent.
// The port's starts transparent instead, so the map lies over the blurred game. ponytail: point-samples the level instead of stretching block by block, so cell edges may round a pixel differently.
Gaf::FFrame RenderLevelMap(const FLevel& Level, const FMapBox& Box)
{
    constexpr uint16_t MapColorKey = 0x800;
    const uint32_t Width = uint32_t(Box.Width);
    const uint32_t Height = uint32_t(Box.Height);
    Gaf::FFrame Map{ .Width = Width, .Height = Height, .ColorKey = MapColorKey, .Pixels = std::vector<uint16_t>(size_t(Width) * Height, MapColorKey) };
    const uint64_t MapLeft = uint64_t(Box.WorldLeft);
    const uint64_t MapTop = uint64_t(Box.WorldTop);
    const uint64_t MapWidth = uint64_t(Box.WorldWidth);
    const uint64_t MapHeight = uint64_t(Box.WorldHeight);
    if (MapWidth == 0 || MapHeight == 0 || Width == 0 || Height == 0)
    {
        return Map;
    }

    for (uint32_t Y = 0; Y < Height; ++Y)
    {
        const uint64_t WorldY = MapTop + Y * MapHeight / Height;
        for (uint32_t X = 0; X < Width; ++X)
        {
            const uint64_t WorldX = MapLeft + X * MapWidth / Width;
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

            // A generator's Shadow-gaf also holds its light as animation 1, beside the shadow its Shadow-index names.
            if (bShadow && Object.SubType == GeneratorKind && !GafAnimations.front().Frames.empty())
            {
                FObjectSprite Light = Result.Sprites.back();
                Light.bShadow = false;
                Light.bGeneratorLight = true;
                Light.Frame = GafAnimations.front().Frames.front();
                Result.Sprites.push_back(Light);
            }

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
        // The light stands where the body does, 90 rows deeper (depth + 0x168), and shows only while its generator is closed.
        if (Sprite.bGeneratorLight)
        {
            const FLevelObject& Object = Level.Objects[Sprite.ObjectIndex];
            const FPlacedAnimation* const Light = PlacedOf[Sprite.ObjectIndex];
            const std::vector<Gaf::FAnimationFrame>& Frames = Sprites.Animations[Sprite.SheetIndex].front().Frames;
            const uint32_t Frame = !Light || Light->bHidden || Light->Playback.bEnded ? 0 : Light->Playback.Frame;
            Sprite.Frame = Frame >= 1 && Frame <= Frames.size() ? Frames[Frame - 1] : Gaf::FAnimationFrame{};
            Sprite.Blend = FrameBlend(Sprite.Frame);
            constexpr int64_t LightDepthBias = 0x168;
            const int64_t GroundY = SpriteCoordinate(Object.Y);
            Sprite.X = float(SpriteCoordinate(Object.X));
            Sprite.Y = float(GroundY) - Object.Altitude - BodyLift(Object, false);
            Sprite.Depth = SpriteDepth(GroundY, false, Object.bInGround, Object.bZShadow) + LightDepthBias;
            continue;
        }

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
// The original ran fullscreen at 640x480; the port fills the monitor at its desktop mode, the 4:3 picture letterboxed, or runs in a window.
// The window's place and size while windowed, kept across a fullscreen spell.
struct FWindowedPlace
{
    int X = 0;
    int Y = 0;
    int Width = int(ScreenWidth) * 2;
    int Height = int(ScreenHeight) * 2;
};
FWindowedPlace WindowedPlace;
bool bFullscreenKeyDown = false;

// The monitor holding the window's centre, else the primary one.
GLFWmonitor* WindowMonitor(GLFWwindow* const Window)
{
    int Count = 0;
    GLFWmonitor** const Monitors = glfwGetMonitors(&Count);
    int X = 0;
    int Y = 0;
    int Width = 0;
    int Height = 0;
    glfwGetWindowPos(Window, &X, &Y);
    glfwGetWindowSize(Window, &Width, &Height);
    for (GLFWmonitor* const Monitor : std::span(Monitors, size_t(std::max(Count, 0))))
    {
        int Left = 0;
        int Top = 0;
        const GLFWvidmode* const Mode = glfwGetVideoMode(Monitor);
        glfwGetMonitorPos(Monitor, &Left, &Top);
        const int CentreX = X + Width / 2;
        const int CentreY = Y + Height / 2;
        if (Mode && CentreX >= Left && CentreX < Left + Mode->width && CentreY >= Top && CentreY < Top + Mode->height)
        {
            return Monitor;
        }
    }

    return glfwGetPrimaryMonitor();
}

void SetFullscreen(GLFWwindow* const Window, const bool bFullscreen)
{
    if ((glfwGetWindowMonitor(Window) != nullptr) == bFullscreen)
    {
        return;
    }

    if (bFullscreen)
    {
        GLFWmonitor* const Monitor = WindowMonitor(Window);
        const GLFWvidmode* const Mode = Monitor ? glfwGetVideoMode(Monitor) : nullptr;
        if (!Mode)
        {
            return;
        }

        // Wayland gives no window position; the window then comes back wherever the compositor puts it.
        if (glfwGetPlatform() != GLFW_PLATFORM_WAYLAND)
        {
            glfwGetWindowPos(Window, &WindowedPlace.X, &WindowedPlace.Y);
        }

        glfwGetWindowSize(Window, &WindowedPlace.Width, &WindowedPlace.Height);
        glfwSetWindowMonitor(Window, Monitor, 0, 0, Mode->width, Mode->height, Mode->refreshRate);
    }
    else
    {
        glfwSetWindowMonitor(Window, nullptr, WindowedPlace.X, WindowedPlace.Y, WindowedPlace.Width, WindowedPlace.Height, GLFW_DONT_CARE);
    }

    bCursorToCapture = true;
}

bool IsAltDown(GLFWwindow* const Window)
{
    return glfwGetKey(Window, GLFW_KEY_LEFT_ALT) == GLFW_PRESS || glfwGetKey(Window, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS;
}

// Alt+Enter or F11 switches between fullscreen and a window, wherever the game is.
void StepFullscreenKey(GLFWwindow* const Window)
{
    const bool bDown = glfwGetKey(Window, GLFW_KEY_F11) == GLFW_PRESS ||
        (IsAltDown(Window) && (glfwGetKey(Window, GLFW_KEY_ENTER) == GLFW_PRESS || glfwGetKey(Window, GLFW_KEY_KP_ENTER) == GLFW_PRESS));
    if (bDown && !bFullscreenKeyDown)
    {
        SetFullscreen(Window, glfwGetWindowMonitor(Window) == nullptr);
    }

    bFullscreenKeyDown = bDown;
}

void PollEvents(GLFWwindow* const Window)
{
    glfwPollEvents();
    StepFullscreenKey(Window);
    StepMouseWheel();
    StepGamepad(ClockMilliseconds());
    // A Wayland window is mapped, and can take the focus, only after its first presented frame, so it is never inactive before its first focus.
    static bool bFocusedOnce = false;
    bFocusedOnce = bFocusedOnce || glfwGetWindowAttrib(Window, GLFW_FOCUSED) != 0;
    const auto IsInactive = [Window]
    {
        return !glfwWindowShouldClose(Window)
            && (glfwGetWindowAttrib(Window, GLFW_ICONIFIED) != 0 || (bFocusedOnce && glfwGetWindowAttrib(Window, GLFW_FOCUSED) == 0));
    };
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
        Down.push_back(IsGamepadConfirmDown());
        return Down;
    }

    GLFWwindow* Window;
    std::vector<bool> Previous;
};

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
    // A fresh press, so the fire held as the player died does not skip it.
    FInputPress AnyInput(Window);
    const uint32_t Start = ClockMilliseconds();
    while (!glfwWindowShouldClose(Window) && ClockMilliseconds() - Start < WaitMilliseconds)
    {
        PollEvents(Window);
        if (AnyInput.Poll())
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
        Down.push_back(IsGamepadConfirmDown());
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

// ShowNetworkFailure (00446120) when the finale ends: over a black screen the lines of PTR_s_____GENERAL_NETWORK_FAILURE_____0050d740 type out at 100, from 100 and 20 apart, one a second, until 10 seconds pass or a key or button goes down.
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
                .Text = std::string(Localize(Lines[Line])),
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

FLetterbox WindowLetterbox(GLFWwindow* const Window, const float ViewWidth)
{
    int Width = 0;
    int Height = 0;
    glfwGetWindowSize(Window, &Width, &Height);
    const double Scale = std::min(Width / double(ViewWidth), Height / double(ScreenHeight));
    return Scale <= 0.0 ? FLetterbox{} : FLetterbox{ .Scale = Scale, .Left = (Width - ViewWidth * Scale) / 2, .Top = (Height - ScreenHeight * Scale) / 2 };
}

// The mouse in the 640x480 screen, or the level's wide view, kept inside it as FUN_00451950 keeps the cursor.
std::pair<int32_t, int32_t> ScreenCursor(GLFWwindow* const Window, const float ViewWidth = ScreenWidth)
{
    const FLetterbox Box = WindowLetterbox(Window, ViewWidth);
    double X = 0.0;
    double Y = 0.0;
    glfwGetCursorPos(Window, &X, &Y);
    return {
        std::clamp(int32_t(std::floor((X - Box.Left) / Box.Scale)), 0, int32_t(ViewWidth) - 1),
        std::clamp(int32_t(std::floor((Y - Box.Top) / Box.Scale)), 0, int32_t(ScreenHeight) - 1),
    };
}

void SetScreenCursor(GLFWwindow* const Window, const int32_t X, const int32_t Y)
{
    constexpr double PixelCentre = 0.5;
    const FLetterbox Box = WindowLetterbox(Window, ScreenWidth);
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
// The original keeps where a menu's last run left the cursor (menu +0x20) to start it there again; the port starts at the pointer.
// OnStep runs after each step and OnDraw after each frame's menu, as the menu's own function (menu +0x6c) does in both passes of MenuStep.
std::optional<size_t> RunMenu(GLFWwindow* const Window, FRenderer& Renderer, const FMenuArt& Art, const FMenuSheets& Sheets, const FMenu& Menu, FRandom& Random,
    std::optional<std::pair<int32_t, int32_t>>& Cursor, const std::function<void(const FMenuState&)>& OnStep = {},
    const std::function<void(const FMenuState&)>& OnDraw = {})
{
    // RunMenu gives the gamma ramp the user's gamma back.
    Renderer.ResetGamma();
    FMenuState State = Art.Start(Menu);
    // RunMenu moves the mouse to the starting item; the port leaves the pointer where it is, and only the arrow keys move it to an item.
    // After the pad, the menu starts on its start item instead, highlighted.
    if (IsGamepadLastUsed())
    {
        FMenuArt::FocusStartItem(Menu, State);
        SetScreenCursor(Window, State.CursorX, State.CursorY);
    }
    else
    {
        std::tie(State.CursorX, State.CursorY) = ScreenCursor(Window);
    }

    // Wayland cannot move the pointer, so the menu's cursor follows the pointer only when the pointer itself moves.
    std::pair<int32_t, int32_t> Pointer = ScreenCursor(Window);
    std::pair<int32_t, int32_t> Mouse(State.CursorX, State.CursorY);
    // PgPoll reports presses, so a key held from before the menu does nothing.
    // Escape and Backspace choose the default item, Space and Return the selected one, and the left button the one under the cursor.
    static constexpr std::array<int, 7> Keys = { GLFW_KEY_ESCAPE, GLFW_KEY_BACKSPACE, GLFW_KEY_SPACE, GLFW_KEY_ENTER, GLFW_KEY_KP_ENTER, GLFW_KEY_UP, GLFW_KEY_DOWN };
    constexpr size_t MouseButton = Keys.size();
    const auto ReadDown = [Window]
    {
        std::array<bool, Keys.size() + 1> Down{};
        // Alt+Enter switches fullscreen instead of choosing an item.
        const bool bAlt = IsAltDown(Window);
        for (size_t Index = 0; Index < Keys.size(); ++Index)
        {
            const bool bReturn = Keys[Index] == GLFW_KEY_ENTER || Keys[Index] == GLFW_KEY_KP_ENTER;
            Down[Index] = glfwGetKey(Window, Keys[Index]) == GLFW_PRESS && !(bReturn && bAlt);
        }

        Down[MouseButton] = glfwGetMouseButton(Window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        return Down;
    };

    auto Previous = ReadDown();
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
        // The pad: the D-pad or the left stick moves, A chooses and B goes back.
        const FGamepadMenuInput Pad = GamepadMenuInput();
        // Start acts as it comes up, so the release that would open the game menu again is spent here.
        Pending.bCancel = Pending.bCancel || Pressed(0) || Pressed(1) || Pad.bBack || (Menu.bStartCancels && IsGamepadActionDown(EAction::Menu));
        Pending.bAccept = Pending.bAccept || Pressed(2) || Pressed(3) || Pressed(4) || Pad.bAccept;
        Pending.bUp = Pending.bUp || Pressed(5) || Pad.bUp;
        Pending.bDown = Pending.bDown || Pressed(6) || Pad.bDown;
        Pending.bClick = Pending.bClick || Pressed(MouseButton);
        Previous = Down;
        const std::pair<int32_t, int32_t> Now = ScreenCursor(Window);
        const bool bPointerMoved = Now != Pointer;
        Pointer = Now;
        Pending.bMouseMoved = Pending.bMouseMoved || bPointerMoved;
        if (bPointerMoved)
        {
            Mouse = Now;
            NoteMouseUsed();
        }

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
            Pending.bLeftHeld = glfwGetKey(Window, GLFW_KEY_LEFT) == GLFW_PRESS || Pad.bLeftHeld;
            Pending.bRightHeld = glfwGetKey(Window, GLFW_KEY_RIGHT) == GLFW_PRESS || Pad.bRightHeld;
            Pending.bButtonHeld = glfwGetMouseButton(Window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            const std::optional<std::pair<int32_t, int32_t>> Jump = Art.Step(Menu, State, Pending, Random, int32_t(StepMilliseconds));
            if (OnStep)
            {
                OnStep(State);
            }

            if (GameAudio && State.Flashes != Flashes)
            {
                GameAudio->PlayMenuFlash();
            }

            if (Jump)
            {
                SetScreenCursor(Window, Jump->first, Jump->second);
                Mouse = *Jump;
                Pointer = ScreenCursor(Window);
            }

            Pending = { .MouseX = Mouse.first, .MouseY = Mouse.second };
        }

        if (Renderer.BeginFrame())
        {
            for (const FHudSprite& Sprite : Art.Draw(Menu, State))
            {
                DrawHudSprite(Renderer, Sprite.Sheet == EHudSheet::VMenu ? Sheets.VMenu : Sheets.Menu, Sprite);
            }

            if (OnDraw)
            {
                OnDraw(State);
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
// MenuExplode at 00421460 sets off explosions over the menu after "Yes, bye."; the port leaves them out and quits at once.
// ponytail: the bip sounds wait for audio.
// Gaps leave that many empty rows above the answers.
std::optional<bool> Confirm(GLFWwindow* const Window, FRenderer& Renderer, const FMenuArt& Art, const FMenuSheets& Sheets, FRandom& Random, std::string Title,
    std::string Yes, std::string No, const size_t DefaultItem, const size_t Gaps = 0)
{
    constexpr int32_t Indent = 30;
    constexpr int32_t Top = 120;
    constexpr size_t TitleFont = 2;
    FMenu Menu{
        .Left = MenuLeft + Indent,
        .Top = Top,
        .Right = int32_t(ScreenWidth),
        .Bottom = int32_t(ScreenHeight),
        .StepY = MenuStepY,
        .Title = std::move(Title),
        .TitleFont = TitleFont,
        .DefaultItem = Gaps + DefaultItem,
    };
    Menu.Items.resize(Gaps);
    Menu.Items.push_back({ .Text = std::move(Yes) });
    Menu.Items.push_back({ .Text = std::move(No) });
    std::optional<std::pair<int32_t, int32_t>> Cursor;
    const std::optional<size_t> Item = RunMenu(Window, Renderer, Art, Sheets, Menu, Random, Cursor);
    return Item ? std::optional(*Item == Gaps) : std::nullopt;
}

// DifficultyName at 00403310 and the help texts at 0050d3e8.
constexpr std::array<std::string_view, DifficultyLevelCount> DifficultyNames{ "Easy", "Normal", "Hard", "Insane" };
constexpr std::array<std::string_view, DifficultyLevelCount> DifficultyHelps{ "For Beginners Only !!!", "A taste of Hell.", "Hurt me !", "Control Alt Del" };

constexpr uint32_t MillisecondsPerHour = 3600000;
constexpr uint32_t MillisecondsPerMinute = 60000;

// SessionDescribeDefault at 00427ad0: the default session's help text.
std::string SessionHelp(const FSession& Session)
{
    const std::string_view DifficultyText = Localize(DifficultyNames[std::min(Session.DifficultyLevel, DifficultyLevelCount - 1)]);
    return Format("%s   (%d:%d:%d)\nEnergy : %d\nShield : %d\nDifficulty : %s ...\n", Session.LevelName, Session.Clock / MillisecondsPerHour,
        Session.Clock % MillisecondsPerHour / MillisecondsPerMinute, Session.Clock % MillisecondsPerMinute / MillisecondsPerSecond, int32_t(Session.Energy),
        int32_t(Session.Shield), DifficultyText);
}

// SessionChoose at 00427c30: up to seven saved sessions with their level and clock, then Back, the default. Returns the chosen session.
// Its own function draws the thumbnail of the session under the cursor at 8, 150.
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
            .Help = Format("Energy : %d\nShield : %d\nDifficulty : %s ...\n", int32_t(Session.Energy), int32_t(Session.Shield),
                Localize(DifficultyNames[std::min(Session.DifficultyLevel, DifficultyLevelCount - 1)])),
        });
    }

    if (Menu.Items.size() < MaxSessions)
    {
        Menu.Items.emplace_back();
    }

    Menu.Items.push_back({ .Text = "Back" });
    Menu.DefaultItem = Menu.Items.size() - 1;
    std::vector<Gaf::FFrame> Thumbnails;
    std::vector<std::optional<size_t>> ThumbnailOf;
    for (const FSession& Session : Settings | std::views::take(MaxSessions))
    {
        std::optional<std::vector<uint16_t>> Pixels = ReadSessionThumbnail(Session);
        ThumbnailOf.push_back(Pixels ? std::optional(Thumbnails.size()) : std::nullopt);
        if (Pixels)
        {
            // An unused colour key, as the picture is opaque.
            Thumbnails.push_back({ .Width = ThumbnailWidth, .Height = ThumbnailHeight, .ColorKey = UINT16_MAX, .Pixels = std::move(*Pixels) });
        }
    }

    const FSpriteSheet ThumbnailSheet = Thumbnails.empty() ? FSpriteSheet{} : Renderer.CreateSpriteSheet(Thumbnails);
    std::optional<std::pair<int32_t, int32_t>> Cursor = std::pair(int32_t(ScreenWidth) / 2, int32_t(ScreenHeight) / 2);
    const std::optional<size_t> Item = RunMenu(Window, Renderer, Art, Sheets, Menu, Random, Cursor, {}, [&](const FMenuState& State)
    {
        constexpr float ThumbnailX = 8.f;
        constexpr float ThumbnailY = 0x96;
        if (State.UnderCursor && *State.UnderCursor < ThumbnailOf.size() && ThumbnailOf[*State.UnderCursor])
        {
            Renderer.DrawSprite(ThumbnailSheet, *ThumbnailOf[*State.UnderCursor], ThumbnailX, ThumbnailY);
        }
    });
    return Item && *Item < std::min(Settings.size(), MaxSessions) ? Item : std::nullopt;
}

// The credits (ShowCredits at 00402990 with the scroller at 00402400): credits.gaf, and credits-en.txt (credits-fr.txt in Russian) in v-menu.gaf font 4 rising from the bottom of the clip 380, 0 to 639, 479 by half a pixel per step, until it has passed the top or a key or mouse button goes down.
void ShowCredits(GLFWwindow* const Window, FRenderer& Renderer, FResourceArchive& Archive, const FMenuArt& Art)
{
    constexpr int32_t ClipLeft = 380;
    constexpr int32_t ClipRight = 639;
    constexpr int32_t ClipBottom = 479;
    constexpr size_t CreditsFont = 4;
    // The scroller's speed argument, 500, times 0.001.
    constexpr float RisePerStep = 0.5f;
    const std::optional<FObjectGaf> Picture = LoadObjectGaf(Archive, "common\\data\\extra\\credits.gaf");
    const std::optional<std::vector<uint8_t>> TextFile = Archive.Read(std::format("common\\data\\extra\\credits-{}.txt", TextSuffix()));
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
    const std::filesystem::path Path = GameFolder / "vz.sco";
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
    const std::optional<FScoreStrip> Strip = ScoreHighlightStrip(Art, Rank);
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

            if (Strip)
            {
                Renderer.DoubleRect(float(Strip->Left), float(Strip->Top), float(Strip->Right - Strip->Left), float(Strip->Bottom - Strip->Top));
            }

            Renderer.EndFrame();
        }
    }
}

// What ConfigLoad (0042f570) reads from vzconf.txt, the Options menus change and ConfigSave (004303f0) writes back.
struct FSettings
{
    const FDifficulty& Difficulty() const { return Difficulties.Levels[DifficultyLevel]; }
    // The port plays with every device at once from the keyboard's table, which binds keys and mouse buttons alike; the mouse and joypad tables only travel through vzconf.txt.
    const FBindingTable& Controls() const { return Bindings.Keyboard; }

    // The game folder's vzconf.txt, which ConfigSave writes.
    std::filesystem::path ConfigPath;
    std::filesystem::path Folder;
    // session-name: the session folder and the player's name, which the saving plots address.
    std::string PlayerName;
    // use-keyboard, use-joystick and use-mouse (session +0x165c), and absolute-command (option flag 1), which the original's mouse always has.
    // The original plays with one device; the port keeps the choice only to write it back, and steers with the sight whenever the mouse moves.
    EInputDevice Device = EInputDevice::Keyboard;
    bool bAbsolute = false;
    FKeyBindings Bindings = FKeyBindings::Defaults();
    FVideoDetails Details;
    FAudioSettings Audio;
    // gamma = red, green, blue (session +0x27d4): the user's gamma, which every ramp sits on.
    std::array<int32_t, 3> Gamma{};
    // difficulty-level (keyword 0x102, SetDifficulty at 004032c0) picks one of COMMON\DIFFICULTY.3DC's levels.
    FDifficultyTable Difficulties;
    size_t DifficultyLevel = 1;
    // language: 1 English, 2 the other language, which ConfigSave names French. The port prints English either way.
    uint32_t Language = 1;
    // The display lines, which the port keeps as read: the original's driver, device and mode.
    std::string VideoDriver;
    std::string VideoDevice;
    std::array<int32_t, 3> VideoMode{ 640, 480, 16 };
    // ConfigSave writes use-extended-movie-player only when the file had it (option flag 0x100 cleared).
    std::optional<bool> bExtendedMoviePlayer;
    // The Audio menu's sound format (session +0x1030, 0x2a0000 from VzRun): 16 or 8 bits, 22050 or 11025 Hz, stereo or mono. Never saved.
    Sound::FOutputFormat SoundFormat;
};

// FUN_00401060: the names vzconf.txt gives the languages and voice types.
std::string_view LanguageKeyword(const uint32_t Language)
{
    constexpr uint32_t SecondLanguage = 2;
    return Language == SecondLanguage ? "French" : "English";
}

// The name editor's buffer holds 16 bytes with the caret and the terminator.
constexpr size_t MaxPlayerNameLength = 14;

// A name that can be a session folder.
bool IsValidPlayerName(const std::string_view Name)
{
    return !Name.empty() && Name.find_first_of("\\/:*?\"<>|.") == std::string_view::npos;
}

// The port's default player name: the OS login name in place of the original's Jack, cut to what the name editor allows.
std::string LoginPlayerName()
{
#ifdef _WIN32
    constexpr const char* LoginVariable = "USERNAME";
#else
    constexpr const char* LoginVariable = "USER";
#endif
    const char* const Login = std::getenv(LoginVariable);
    const std::string Name = Login ? std::string(Login).substr(0, MaxPlayerNameLength) : std::string();
    return IsValidPlayerName(Name) ? Name : std::string("Player");
}

// ConfigLoad (0042f570) reads vzconf.txt from the game folder, else the archive's VZCONF.TXT. A line is a keyword, an optional '=' and a value; a flag is set when its value reads as a non-zero number (atoi).
// Keyboard-Settings, Mouse-Settings and Joypad-Settings start a device's bindings, each line an action keyword and its code and modifier.
FSettings LoadSettings(FResourceArchive& Archive, const std::filesystem::path& ConfigFolder)
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
    const auto Number = [](const std::string_view Value)
    {
        int Result = 0;
        std::from_chars(Value.data(), Value.data() + Value.size(), Result);
        return Result;
    };
    constexpr std::string_view Blanks = " \t\r";
    const auto Trim = [&](std::string_view Value)
    {
        Value.remove_prefix(std::min(Value.size(), Value.find_first_not_of(Blanks)));
        return Value.substr(0, Value.find_last_not_of(Blanks) + 1);
    };
    // "%d, %d, %d" and the like.
    const auto Numbers = [&](const std::string_view Value, const std::span<int32_t> Out)
    {
        size_t Count = 0;
        for (const auto Field : std::views::split(Value, ','))
        {
            const std::string_view Part = Trim(std::string_view(Field.begin(), Field.end()));
            if (Count < Out.size() && std::from_chars(Part.data(), Part.data() + Part.size(), Out[Count]).ec == std::errc{})
            {
                ++Count;
            }
        }

        return Count == Out.size();
    };
    const auto Lower = [](std::string_view Text)
    {
        std::string Result(Text);
        std::ranges::transform(Result, Result.begin(), [](const unsigned char Character) { return char(std::tolower(Character)); });
        return Result;
    };

    FSettings Settings{ .ConfigPath = ConfigFolder / "vzconf.txt", .PlayerName = LoginPlayerName() };
    const std::optional<std::vector<uint8_t>> DifficultyFile = Archive.Read("common\\difficulty.3dc");
    Settings.Difficulties = LoadDifficulties(DifficultyFile ? std::string_view(reinterpret_cast<const char*>(DifficultyFile->data()), DifficultyFile->size()) : std::string_view());
    Settings.DifficultyLevel = Settings.Difficulties.DefaultLevel;
    const std::optional<std::vector<uint8_t>> Config = Archive.Read("VZCONF.TXT");
    std::string Text = Config ? std::string(reinterpret_cast<const char*>(Config->data()), Config->size()) : std::string();
    std::error_code Error;
    bool bFolderConfig = false;
    for (const std::filesystem::directory_entry& Entry : std::filesystem::directory_iterator(ConfigFolder, Error))
    {
        constexpr std::uintmax_t MaxConfigBytes = 65536;
        if (Lower(Entry.path().filename().string()) == "vzconf.txt" && Entry.file_size(Error) <= MaxConfigBytes)
        {
            Settings.ConfigPath = Entry.path();
            bFolderConfig = true;
            std::ifstream File(Entry.path(), std::ios::binary);
            Text.assign(std::istreambuf_iterator<char>(File), std::istreambuf_iterator<char>());
        }
    }

    std::optional<EInputDevice> BindingDevice;
    for (const auto Part : std::views::split(std::string_view(Text), '\n'))
    {
        const std::string_view Row = Trim(std::string_view(Part.begin(), Part.end()));
        if (Row.empty() || Row.starts_with('#'))
        {
            continue;
        }

        constexpr std::string_view KeyEnd = " \t=";
        const std::string Key = Lower(Row.substr(0, Row.find_first_of(KeyEnd)));
        std::string_view Value = Trim(Row.substr(std::min(Row.size(), Row.find_first_of(KeyEnd))));
        if (Value.starts_with('='))
        {
            Value = Trim(Value.substr(1));
        }

        const bool bSet = Number(Value) != 0;
        if (Key == "session-name" && IsValidPlayerName(Value))
        {
            Settings.PlayerName = std::string(Value);
        }
        else if (int Level = 0; Key == "difficulty-level" && std::from_chars(Value.data(), Value.data() + Value.size(), Level).ec == std::errc{} && Level >= 0 &&
            Level < int(DifficultyLevelCount))
        {
            Settings.DifficultyLevel = size_t(Level);
        }
        else if (Key == "gamma")
        {
            Numbers(Value, Settings.Gamma);
        }
        else if (Key == "absolute-command")
        {
            Settings.bAbsolute = bSet;
        }
        else if (Key == "use-keyboard" || Key == "use-joystick" || Key == "use-mouse")
        {
            if (bSet)
            {
                Settings.Device = Key == "use-mouse" ? EInputDevice::Mouse : Key == "use-joystick" ? EInputDevice::Joypad : EInputDevice::Keyboard;
            }
        }
        else if (Key == "language" || Key == "voice-type")
        {
            constexpr uint32_t SecondLanguage = 2;
            (Key == "language" ? Settings.Language : Settings.Audio.VoiceType) = Lower(Value) == "french" ? SecondLanguage : 1;
        }
        else if (Key == "video-driver" || Key == "video-driver-device")
        {
            (Key == "video-driver" ? Settings.VideoDriver : Settings.VideoDevice) = std::string(Value);
        }
        else if (std::array<int32_t, 3> Mode{}; Key == "video-driver-device-mode" && Numbers(Value, Mode))
        {
            Settings.VideoMode = Mode;
        }
        else if (Key == "use-extended-movie-player")
        {
            Settings.bExtendedMoviePlayer = bSet;
        }
        else if (Key == "keyboard-settings" || Key == "mouse-settings" || Key == "joypad-settings")
        {
            BindingDevice = Key == "mouse-settings" ? EInputDevice::Mouse : Key == "joypad-settings" ? EInputDevice::Joypad : EInputDevice::Keyboard;
        }
        else if (const auto Detail = std::ranges::find(DetailKeys, Key, &FDetailKey::Name); Detail != DetailKeys.end())
        {
            Settings.Details.*Detail->Field = bSet != Detail->bInverted;
        }
        else if (const auto Switch = std::ranges::find(AudioSwitches, Key, &FAudioSwitch::Name); Switch != AudioSwitches.end())
        {
            Settings.Audio.*Switch->Field = bSet;
        }
        else if (const auto Volume = std::ranges::find(AudioVolumes, Key, &FAudioVolume::Name); Volume != AudioVolumes.end())
        {
            // ConfigLoad clamps the volumes to 0..0xffff.
            Settings.Audio.*Volume->Field = std::clamp(Number(Value), 0, Sound::MaxVolume);
        }
        else if (BindingDevice)
        {
            for (size_t Action = 1; Action < ActionCount; ++Action)
            {
                if (std::array<int32_t, 2> Binding{}; Key == ActionKeyword(Action) && Numbers(Value, Binding))
                {
                    Settings.Bindings.Bind(*BindingDevice, EAction(Action), { .Code = uint32_t(Binding[0]), .Modifier = uint32_t(Binding[1]) });
                }
            }
        }
    }

    // The archive's VZCONF.TXT starts in the absolute direction, where the mouse cannot steer; the port's mouse layout steers relative to the ship, so a fresh game starts relative.
    if (!bFolderConfig)
    {
        Settings.bAbsolute = false;
    }

    // The archive's VZCONF.TXT and every vzconf.txt the original writes before a rename name the player Jack.
    if (Settings.PlayerName == "Jack")
    {
        Settings.PlayerName = LoginPlayerName();
    }

    return Settings;
}

// ConfigSave at 004303f0, as the Options menu closes, in its order and layout; then the bindings that differ from a device's defaults (FUN_00430240).
void SaveSettings(const FSettings& Settings)
{
    std::string Text = "#\n# Vyruz configuration options.\n#\n\n# * Control\n#\n";
    const auto Line = [&](const std::string_view Key, const int Value) { Text += std::format("{}\t= {}\n", Key, Value); };
    const auto Triple = [&](const std::string_view Key, const std::array<int32_t, 3>& Values)
    {
        Text += std::format("{}\t = {}, {}, {}\n", Key, Values[0], Values[1], Values[2]);
    };
    Line("absolute-command", Settings.bAbsolute);
    Text += Settings.Device == EInputDevice::Mouse ? "use-mouse = 1\n" : Settings.Device == EInputDevice::Joypad ? "use-joystick = 1\n" : "use-keyboard = 1\n";
    Text += "\n# * Audio\n#\n";
    Line("music", Settings.Audio.bMusic);
    Line("sound-fx", Settings.Audio.bSoundFx);
    Line("voices", Settings.Audio.bVoices);
    Line("music-volume", Settings.Audio.MusicVolume);
    Line("sound-fx-volume", Settings.Audio.SoundFxVolume);
    Line("voices-volume", Settings.Audio.VoicesVolume);
    Text += "\n# * Video, Graphic display\n#\n";
    Line("shadow", Settings.Details.bShadows);
    Line("reactor-fx", Settings.Details.bReactorFx);
    Line("hud-green-info", Settings.Details.bGreenInfo);
    Line("translucent-panel", Settings.Details.bTranslucentPanel);
    Line("tv-borders", Settings.Details.bTvBorders);
    Line("low-details", Settings.Details.bLowDetails);
    Line("gamma-fx", Settings.Details.bGammaFx);
    Triple("gamma", Settings.Gamma);
    Text += std::format("video-driver\t= {}\nvideo-driver-device\t= {}\n", Settings.VideoDriver, Settings.VideoDevice);
    Triple("video-driver-device-mode", Settings.VideoMode);
    if (Settings.bExtendedMoviePlayer)
    {
        Line("use-extended-movie-player", *Settings.bExtendedMoviePlayer);
    }

    Line("difficulty-level", int(Settings.DifficultyLevel));
    Text += std::format("session-name\t= {}\nlanguage\t= {}\nvoice-type\t= {}\n", Settings.PlayerName, LanguageKeyword(Settings.Language),
        LanguageKeyword(Settings.Audio.VoiceType));
    for (const EInputDevice Device : { EInputDevice::Keyboard, EInputDevice::Mouse, EInputDevice::Joypad })
    {
        bool bHeader = false;
        for (size_t Action = 0; Action < ActionCount; ++Action)
        {
            const FBinding& Binding = Settings.Bindings.Table(Device)[Action];
            const FBinding& Default = FKeyBindings::DefaultTable(Device)[Action];
            if ((Binding.Code & BindingCodeMask) == (Default.Code & BindingCodeMask) && (Binding.Modifier & BindingCodeMask) == (Default.Modifier & BindingCodeMask))
            {
                continue;
            }

            if (!std::exchange(bHeader, true))
            {
                Text += std::format("{}\n", DeviceKeyword(Device));
            }

            Text += std::format("{}\t = {}, {}\n", ActionKeyword(Action), Binding.Code & BindingCodeMask, Binding.Modifier & BindingCodeMask);
        }
    }

    if (!WriteFileText(Settings.ConfigPath, Text))
    {
        std::fprintf(stderr, "Cannot write %s\n", Settings.ConfigPath.string().c_str());
    }
}

// The menus behind Options (MenuOptions at 004229a0 and the menus it opens): each over the main menus' rectangle, titled Options with the path to it after.
FMenu OptionsMenu(std::string Path, std::string SubPath)
{
    return {
        .Left = MenuLeft,
        .Top = MenuTop,
        .Right = int32_t(ScreenWidth),
        .Bottom = int32_t(ScreenHeight),
        .StepY = MenuStepY,
        .Title = "Options",
        .Path = std::move(Path),
        .SubPath = std::move(SubPath),
    };
}

// The lists among them: 50 pixels right of and 16 above the menu, items 35 apart in fonts 4 and 3.
FMenu OptionsList(std::string Path, std::string SubPath)
{
    constexpr int32_t ListIndent = 0x32;
    constexpr int32_t ListRise = 0x10;
    constexpr int32_t ListStepY = 0x23;
    constexpr size_t ListHighlightFont = 4;
    constexpr size_t ListItemFont = 3;
    FMenu Menu = OptionsMenu(std::move(Path), std::move(SubPath));
    Menu.Left += ListIndent;
    Menu.Top -= ListRise;
    Menu.StepY = ListStepY;
    Menu.HighlightFont = ListHighlightFont;
    Menu.ItemFont = ListItemFont;
    return Menu;
}

// FUN_00420d10: Back ends every one of these menus and is what Escape chooses.
void AddBack(FMenu& Menu, std::string Help = {})
{
    Menu.Items.push_back({ .Text = "Back", .Help = std::move(Help) });
    Menu.DefaultItem = Menu.Items.size() - 1;
}

bool IsBack(const std::optional<size_t> Item, const FMenu& Menu)
{
    return !Item || Item == Menu.DefaultItem;
}

// FUN_00423b20: a Video Details Custom item.
std::string OnOffText(const std::string_view Name, const bool bOn)
{
    return std::format("{} : {}", Localize(Name), Localize(bOn ? "ON" : "OFF"));
}

// Steps and draws a menu without taking its input until Done says so, as the key settings' wait (FUN_00425860) and the text editor (FUN_00425f60) run MenuStep. False when the window closes.
bool ShowMenuUntil(GLFWwindow* const Window, FRenderer& Renderer, const FMenuArt& Art, const FMenuSheets& Sheets, const FMenu& Menu, FRandom& Random,
    const std::function<bool(uint32_t)>& Done)
{
    FMenuState State = Art.Start(Menu);
    const uint32_t FramePeriod = DisplayFramePeriod();
    FFrameClock Clock(FramePeriod);
    while (!glfwWindowShouldClose(Window))
    {
        PollEvents(Window);
        const std::pair<int32_t, int32_t> Mouse = ScreenCursor(Window);
        const uint32_t Elapsed = Clock.Poll(ClockMilliseconds());
        for (uint32_t Remaining = Elapsed; Remaining != 0;)
        {
            const uint32_t StepMilliseconds = NextGameStep(Remaining, FramePeriod, false);
            Art.Step(Menu, State, { .MouseX = Mouse.first, .MouseY = Mouse.second }, Random, int32_t(StepMilliseconds));
        }

        if (Done(Elapsed))
        {
            return true;
        }

        if (Renderer.BeginFrame())
        {
            for (const FHudSprite& Sprite : Art.Draw(Menu, State))
            {
                DrawHudSprite(Renderer, Sprite.Sheet == EHudSheet::VMenu ? Sheets.VMenu : Sheets.Menu, Sprite);
            }

            Renderer.EndFrame();
        }
    }

    return false;
}

// The characters typed since the editor last looked, from GLFW's character callback.
std::string TypedCharacters;

// FUN_00425f60 edits an item's text after Prefix, at most MaxLength characters, behind a caret that blinks between | and . every half second.
// Return accepts a text with a letter or digit, its trailing blanks dropped; Escape leaves the old one. Returns the new text when it differs from the old.
std::optional<std::string> EditText(GLFWwindow* const Window, FRenderer& Renderer, const FMenuArt& Art, const FMenuSheets& Sheets, FMenu Menu, FRandom& Random,
    const size_t Item, const std::string_view Prefix, const std::string_view Old, const size_t MaxLength)
{
    // The item shows 20 characters, prefix included, scrolling to keep the caret in view.
    constexpr size_t VisibleCharacters = 0x14;
    constexpr int32_t BlinkMilliseconds = 500;
    std::string Text(Old);
    size_t Caret = Text.size();
    size_t Scroll = 0;
    char CaretGlyph = '|';
    int32_t Blink = BlinkMilliseconds;
    bool bTyped = false;
    std::optional<std::string> Result;
    bool bCancelled = false;
    static constexpr std::array<int, 8> EditKeys{ GLFW_KEY_ESCAPE, GLFW_KEY_ENTER, GLFW_KEY_KP_ENTER, GLFW_KEY_BACKSPACE, GLFW_KEY_DELETE, GLFW_KEY_HOME, GLFW_KEY_END, GLFW_KEY_LEFT };
    std::array<bool, EditKeys.size() + 1> WasDown{};
    const auto KeysDown = [&]
    {
        std::array<bool, EditKeys.size() + 1> Down{};
        for (size_t Index = 0; Index < EditKeys.size(); ++Index)
        {
            Down[Index] = glfwGetKey(Window, EditKeys[Index]) == GLFW_PRESS;
        }

        Down.back() = glfwGetKey(Window, GLFW_KEY_RIGHT) == GLFW_PRESS;
        return Down;
    };
    WasDown = KeysDown();
    TypedCharacters.clear();
    const GLFWcharfun PreviousChar = glfwSetCharCallback(Window, [](GLFWwindow*, const unsigned int Character)
    {
        constexpr unsigned int FirstPrintable = 0x20;
        constexpr unsigned int LastPrintable = 0x7e;
        if (Character >= FirstPrintable && Character <= LastPrintable)
        {
            TypedCharacters += char(Character);
        }
    });
    const auto Refresh = [&]
    {
        const std::string Shown = std::string(Prefix) + Text.substr(0, Caret) + CaretGlyph + Text.substr(Caret);
        const size_t CaretAt = Prefix.size() + Caret;
        Scroll = CaretAt >= Scroll + VisibleCharacters ? CaretAt + 1 - VisibleCharacters : std::min(Scroll, CaretAt);
        Menu.Items[Item].Text = Shown.substr(Scroll, VisibleCharacters);
        Menu.Items[Item].Help = bTyped ? std::string(Localize("Press ENTER to validate : ")) + Text : "Edit your string.";
    };
    Refresh();
    const bool bOpen = ShowMenuUntil(Window, Renderer, Art, Sheets, Menu, Random, [&](const uint32_t Elapsed)
    {
        const auto Down = KeysDown();
        const auto Pressed = [&](const size_t Index) { return Down[Index] && !WasDown[Index]; };
        bool bEdited = !TypedCharacters.empty();
        for (const char Character : std::exchange(TypedCharacters, {}))
        {
            if (Text.size() < MaxLength && std::string_view("\\/:*?\"<>|.").find(Character) == std::string_view::npos)
            {
                Text.insert(Caret++, 1, Character);
            }
        }

        constexpr size_t EscapeKey = 0;
        constexpr size_t ReturnKey = 1;
        constexpr size_t PadReturnKey = 2;
        constexpr size_t BackspaceKey = 3;
        constexpr size_t DeleteKey = 4;
        constexpr size_t HomeKey = 5;
        constexpr size_t EndKey = 6;
        constexpr size_t LeftKey = 7;
        const size_t RightKey = Down.size() - 1;
        if (Pressed(EscapeKey))
        {
            bCancelled = true;
        }
        else if (Pressed(ReturnKey) || Pressed(PadReturnKey))
        {
            if (std::ranges::any_of(Text, [](const unsigned char Character) { return std::isalnum(Character) != 0; }))
            {
                Text.erase(Text.find_last_not_of(" \t") + 1);
                Result = Text;
            }
        }
        else if (Pressed(BackspaceKey) && Caret > 0)
        {
            Text.erase(--Caret, 1);
        }
        else if (Pressed(DeleteKey) && Caret < Text.size())
        {
            Text.erase(Caret, 1);
        }
        else if (Pressed(HomeKey))
        {
            Caret = 0;
        }
        else if (Pressed(EndKey))
        {
            Caret = Text.size();
        }
        else if (Pressed(LeftKey) && Caret > 0)
        {
            --Caret;
        }
        else if (Pressed(RightKey) && Caret < Text.size())
        {
            ++Caret;
        }

        bEdited = bEdited || std::ranges::any_of(std::views::iota(size_t(0), Down.size()), Pressed);
        WasDown = Down;
        bTyped = bTyped || bEdited;
        Blink -= int32_t(Elapsed);
        if (Blink < 0)
        {
            Blink += BlinkMilliseconds;
            CaretGlyph = CaretGlyph == '|' ? '.' : '|';
        }

        Refresh();
        return bCancelled || Result.has_value();
    });
    glfwSetCharCallback(Window, PreviousChar);
    return bOpen && Result && *Result != Old ? Result : std::nullopt;
}

// A slider menu (the gamma and the volumes): a label over five gaps and Back; Apply takes the value at every step and once more as it closes.
// Returns the last value.
std::optional<int32_t> RunSlider(GLFWwindow* const Window, FRenderer& Renderer, const FMenuArt& Art, const FMenuSheets& Sheets, FRandom& Random, std::string Path,
    std::string SubPath, std::string Label, const int32_t Value, const std::function<void(int32_t)>& Apply)
{
    constexpr size_t SliderGaps = 5;
    FMenu Menu = OptionsMenu(std::move(Path), std::move(SubPath));
    Menu.Items.resize(SliderGaps);
    AddBack(Menu);
    Menu.bSlider = true;
    Menu.SliderLabel = std::move(Label);
    Menu.SliderValue = Value;
    std::optional<std::pair<int32_t, int32_t>> Cursor;
    int32_t Last = Value;
    std::optional<size_t> Item;
    do
    {
        Item = RunMenu(Window, Renderer, Art, Sheets, Menu, Random, Cursor, [&](const FMenuState& State)
        {
            Last = State.SliderValue;
            Apply(Last);
        });
        Menu.SliderValue = Last;
    } while (Item && !IsBack(Item, Menu));

    Apply(Last);
    return Item ? std::optional(Last) : std::nullopt;
}

// MenuVideoDetailsCustom at 00423760: each detail flag ON or OFF; Ground/Smoke details shows flag 0x40 inverted.
void RunDetailsCustomMenu(GLFWwindow* const Window, FRenderer& Renderer, const FMenuArt& Art, const FMenuSheets& Sheets, FRandom& Random, FSettings& Settings)
{
    struct FDetailItem
    {
        std::string_view Name;
        bool FVideoDetails::* Field = nullptr;
        bool bInverted = false;
    };
    constexpr std::array Details{
        FDetailItem{ .Name = "Shadow", .Field = &FVideoDetails::bShadows },
        FDetailItem{ .Name = "Ground/Smoke details", .Field = &FVideoDetails::bLowDetails, .bInverted = true },
        FDetailItem{ .Name = "Reactor FX", .Field = &FVideoDetails::bReactorFx },
        FDetailItem{ .Name = "HUD Green Info", .Field = &FVideoDetails::bGreenInfo },
        FDetailItem{ .Name = "Translucent Panel", .Field = &FVideoDetails::bTranslucentPanel },
        FDetailItem{ .Name = "TV Borders", .Field = &FVideoDetails::bTvBorders },
        FDetailItem{ .Name = "Gamma FX", .Field = &FVideoDetails::bGammaFx },
    };
    FMenu Menu = OptionsList("/Video/Details", "/Custom");
    for (const FDetailItem& Detail : Details)
    {
        Menu.Items.push_back({ .Text = OnOffText(Detail.Name, Settings.Details.*Detail.Field != Detail.bInverted) });
    }

    AddBack(Menu);
    std::optional<std::pair<int32_t, int32_t>> Cursor;
    for (std::optional<size_t> Item; !IsBack(Item = RunMenu(Window, Renderer, Art, Sheets, Menu, Random, Cursor), Menu);)
    {
        const FDetailItem& Detail = Details[*Item];
        bool& bFlag = Settings.Details.*Detail.Field;
        bFlag = !bFlag;
        Menu.Items[*Item].Text = OnOffText(Detail.Name, bFlag != Detail.bInverted);
    }
}

// FUN_00423500: the Details help.
std::string DetailsHelp(const FSettings& Settings)
{
    return std::string(Localize("Current Graphic Details : ")) + std::string(Localize(Settings.Details.bLowDetails ? "Low." : "High."));
}

// MenuVideoDetails at 004235a0: High sets flags 0xbe and clears 0x40, Low clears 0xae and sets 0x40 (Translucent Panel stays), and either closes the menu; Custom opens the flags.
void RunDetailsMenu(GLFWwindow* const Window, FRenderer& Renderer, const FMenuArt& Art, const FMenuSheets& Sheets, FRandom& Random, FSettings& Settings)
{
    enum EItem : size_t
    {
        HighItem,
        LowItem,
        CustomItem,
    };
    FMenu Menu = OptionsMenu("/Video", "/Details");
    Menu.Items = { { .Text = "High", .Help = DetailsHelp(Settings) }, { .Text = "Low", .Help = DetailsHelp(Settings) }, { .Text = "Custom" }, {}, {} };
    AddBack(Menu);
    std::optional<std::pair<int32_t, int32_t>> Cursor;
    for (std::optional<size_t> Item; !IsBack(Item = RunMenu(Window, Renderer, Art, Sheets, Menu, Random, Cursor), Menu);)
    {
        if (*Item == CustomItem)
        {
            RunDetailsCustomMenu(Window, Renderer, Art, Sheets, Random, Settings);
            continue;
        }

        const bool bHigh = *Item == HighItem;
        FVideoDetails& Details = Settings.Details;
        Details.bShadows = Details.bReactorFx = Details.bGreenInfo = Details.bTvBorders = Details.bGammaFx = bHigh;
        Details.bTranslucentPanel = bHigh || Details.bTranslucentPanel;
        Details.bLowDetails = !bHigh;
        return;
    }
}

// FUN_00422c10, the Video menu: Details, the display's driver, device and mode, and Gamma Correction.
// ponytail: the driver, device and mode name the port's own display; their lists (FUN_00422e20, FUN_00422fb0, FUN_00423150) have nothing else to offer.
void RunVideoMenu(GLFWwindow* const Window, FRenderer& Renderer, const FMenuArt& Art, const FMenuSheets& Sheets, FRandom& Random, FSettings& Settings)
{
    enum EItem : size_t
    {
        DetailsItem,
        GammaItem = 4,
    };
    FMenu Menu = OptionsMenu({}, "/Video");
    Menu.Items = {
        { .Text = "Details Options", .Help = DetailsHelp(Settings) },
        { .Text = "Vulkan" },
        { .Text = Renderer.GetDeviceName() },
        { .Text = std::format("{} x {} x {} bits", int32_t(ScreenWidth), int32_t(ScreenHeight), Settings.VideoMode[2]) },
        { .Text = "Gamma Correction" },
    };
    AddBack(Menu);
    std::optional<std::pair<int32_t, int32_t>> Cursor;
    for (std::optional<size_t> Item; !IsBack(Item = RunMenu(Window, Renderer, Art, Sheets, Menu, Random, Cursor), Menu);)
    {
        if (*Item == DetailsItem)
        {
            RunDetailsMenu(Window, Renderer, Art, Sheets, Random, Settings);
            Menu.Items[DetailsItem].Help = DetailsHelp(Settings);
        }
        else if (*Item == GammaItem)
        {
            // FUN_00423370: the value maps -255..255 onto 0..100; FUN_004234b0 moves all three channels by the change, and the ramp becomes the user's gamma.
            constexpr int32_t GammaSpan = 0x1fe;
            constexpr int32_t GammaOffset = 0xff;
            constexpr int32_t Percent = 100;
            const int32_t Value = (Settings.Gamma[0] + GammaOffset) * Percent / GammaSpan;
            RunSlider(Window, Renderer, Art, Sheets, Random, "/Video", "/Gamma", std::string(Localize("Gamma value : ")), Value, [&](const int32_t Slider)
            {
                const std::array<int32_t, 3>& Ramp = Renderer.GetGamma();
                if (const int32_t Change = Slider * GammaSpan / Percent - Ramp[0] - GammaOffset; Change != 0)
                {
                    Renderer.SetGamma({ Ramp[0] + Change, Ramp[1] + Change, Ramp[2] + Change });
                }
            });
            Settings.Gamma = Renderer.GetGamma();
            Renderer.SetUserGamma(Settings.Gamma);
        }
    }
}

// FUN_004244e0, FUN_00424ac0 and FUN_00424c50: a volume from 0 to 0xffff as 0 to 100.
void RunVolumeMenu(GLFWwindow* const Window, FRenderer& Renderer, const FMenuArt& Art, const FMenuSheets& Sheets, FRandom& Random, FSettings& Settings,
    std::string Path, int FAudioSettings::* const Volume)
{
    constexpr int Percent = 100;
    RunSlider(Window, Renderer, Art, Sheets, Random, std::move(Path), "/Volume", "Volume : ", Settings.Audio.*Volume * Percent / Sound::MaxVolume, [&](const int32_t Value)
    {
        Settings.Audio.*Volume = Value * Sound::MaxVolume / Percent;
        if (GameAudio)
        {
            GameAudio->Apply(Settings.Audio);
        }
    });
}

// FUN_00424280, the music: on or off, and its volume while on.
void RunMusicMenu(GLFWwindow* const Window, FRenderer& Renderer, const FMenuArt& Art, const FMenuSheets& Sheets, FRandom& Random, FSettings& Settings)
{
    enum EItem : size_t
    {
        SwitchItem,
        VolumeItem,
    };
    FMenu Menu = OptionsMenu("/Audio", "/Music");
    const auto Update = [&]
    {
        const bool bOn = Settings.Audio.bMusic;
        Menu.Items[SwitchItem] = { .Text = bOn ? "Music : ON" : "Music : OFF", .Help = bOn ? "Select to desactivate Music." : "Select to activate Music." };
        Menu.Items[VolumeItem] = { .Text = "Tune Music Volume", .Help = bOn ? "" : "Music is desactivated." };
    };
    Menu.Items.resize(5);
    AddBack(Menu);
    Update();
    std::optional<std::pair<int32_t, int32_t>> Cursor;
    for (std::optional<size_t> Item; !IsBack(Item = RunMenu(Window, Renderer, Art, Sheets, Menu, Random, Cursor), Menu);)
    {
        if (*Item == SwitchItem)
        {
            Settings.Audio.bMusic = !Settings.Audio.bMusic;
            if (GameAudio)
            {
                GameAudio->Apply(Settings.Audio);
                GameAudio->SetMusicOn(Settings.Audio.bMusic);
            }

            Update();
        }
        else if (*Item == VolumeItem && Settings.Audio.bMusic)
        {
            RunVolumeMenu(Window, Renderer, Art, Sheets, Random, Settings, "/Audio/Music", &FAudioSettings::MusicVolume);
        }
    }
}

// FUN_00424dc0, the voices' language: English or Russian (voice-type, written English or French).
void RunVoiceTypeMenu(GLFWwindow* const Window, FRenderer& Renderer, const FMenuArt& Art, const FMenuSheets& Sheets, FRandom& Random, FSettings& Settings)
{
    constexpr uint32_t SecondLanguage = 2;
    const std::string Help = Format("Current voice type is %s.", Localize(Settings.Audio.VoiceType == SecondLanguage ? "Russian" : "English"));
    FMenu Menu = OptionsMenu("/Audio/Voices", "/Type");
    Menu.Items = { { .Text = "English", .Help = Help }, { .Text = "Russian", .Help = Help }, {}, {}, {} };
    AddBack(Menu);
    std::optional<std::pair<int32_t, int32_t>> Cursor;
    if (const std::optional<size_t> Item = RunMenu(Window, Renderer, Art, Sheets, Menu, Random, Cursor); !IsBack(Item, Menu))
    {
        Settings.Audio.VoiceType = uint32_t(*Item) + 1;
        if (GameAudio)
        {
            GameAudio->Apply(Settings.Audio);
        }
    }
}

// FUN_004246b0, the sound effects and the voices: each on or off, their volumes and the voice type while on.
// Switching the voices sets the help of the second item, not the fourth, as the original does.
void RunEffectsMenu(GLFWwindow* const Window, FRenderer& Renderer, const FMenuArt& Art, const FMenuSheets& Sheets, FRandom& Random, FSettings& Settings)
{
    enum EItem : size_t
    {
        EffectsItem,
        EffectsVolumeItem,
        VoicesItem,
        VoicesVolumeItem,
        VoiceTypeItem,
    };
    constexpr std::string_view EffectsRemoved = "Sound FX are removed.";
    constexpr std::string_view VoicesRemoved = "Voices are removed.";
    FMenu Menu = OptionsMenu("/Audio", "/FX,Voices");
    const bool bEffects = Settings.Audio.bSoundFx;
    const bool bVoices = Settings.Audio.bVoices;
    Menu.Items = {
        { .Text = bEffects ? "Sound FX : ON" : "Sound FX : OFF", .Help = bEffects ? "Select to remove Sound FX." : "Select to activate Sound FX." },
        { .Text = "Tune Sound FX Volume", .Help = bEffects ? "" : std::string(EffectsRemoved) },
        { .Text = bVoices ? "Voices : ON" : "Voices : OFF", .Help = bVoices ? "Select to remove Voices." : "Select to activate Voices." },
        { .Text = "Tune Voices Volume", .Help = bVoices ? "" : std::string(VoicesRemoved) },
        { .Text = "Voice type", .Help = bVoices ? "" : std::string(VoicesRemoved) },
    };
    AddBack(Menu);
    std::optional<std::pair<int32_t, int32_t>> Cursor;
    for (std::optional<size_t> Item; !IsBack(Item = RunMenu(Window, Renderer, Art, Sheets, Menu, Random, Cursor), Menu);)
    {
        if (*Item == EffectsItem)
        {
            bool& bOn = Settings.Audio.bSoundFx;
            bOn = !bOn;
            Menu.Items[EffectsItem] = { .Text = bOn ? "Sound FX : ON" : "Sound FX : OFF", .Help = bOn ? "Select to remove Sound FX." : "Select to activate Sound FX." };
            Menu.Items[EffectsVolumeItem].Help = bOn ? "" : std::string(EffectsRemoved);
        }
        else if (*Item == VoicesItem)
        {
            bool& bOn = Settings.Audio.bVoices;
            bOn = !bOn;
            Menu.Items[VoicesItem] = { .Text = bOn ? "Voices : ON" : "Voices : OFF", .Help = bOn ? "Select to remove Voices." : "Select to activate Voices." };
            Menu.Items[EffectsVolumeItem].Help = bOn ? "" : std::string(VoicesRemoved);
        }
        else if (*Item == EffectsVolumeItem && Settings.Audio.bSoundFx)
        {
            RunVolumeMenu(Window, Renderer, Art, Sheets, Random, Settings, "/Audio/FX", &FAudioSettings::SoundFxVolume);
        }
        else if (*Item == VoicesVolumeItem && Settings.Audio.bVoices)
        {
            RunVolumeMenu(Window, Renderer, Art, Sheets, Random, Settings, "/Audio/Voices", &FAudioSettings::VoicesVolume);
        }
        else if (*Item == VoiceTypeItem && Settings.Audio.bVoices)
        {
            RunVoiceTypeMenu(Window, Renderer, Art, Sheets, Random, Settings);
        }

        if (GameAudio)
        {
            GameAudio->Apply(Settings.Audio);
        }
    }
}

// FUN_00423e20, the Audio menu: music, effects and voices, and the sound format's quality, frequency and channels.
void RunAudioMenu(GLFWwindow* const Window, FRenderer& Renderer, const FMenuArt& Art, const FMenuSheets& Sheets, FRandom& Random, FSettings& Settings)
{
    enum EItem : size_t
    {
        MusicItem,
        EffectsItem,
        QualityItem,
        FrequencyItem,
        ChannelsItem,
    };
    constexpr int EightBits = 8;
    constexpr int SixteenBits = 16;
    FMenu Menu = OptionsMenu({}, "/Audio");
    const auto Update = [&]
    {
        Menu.Items[QualityItem].Text = Format("Quality : %d bits", Settings.SoundFormat.bEightBits ? EightBits : SixteenBits);
        Menu.Items[FrequencyItem].Text = Format("Frequency : %d Hz", Settings.SoundFormat.Rate);
        Menu.Items[ChannelsItem].Text = Settings.SoundFormat.bMono ? "Channels : Mono" : "Channels : Stereo";
    };
    Menu.Items = { { .Text = "Music" }, { .Text = "Sound FX, Voices" }, {}, {}, {} };
    AddBack(Menu);
    Update();
    std::optional<std::pair<int32_t, int32_t>> Cursor;
    for (std::optional<size_t> Item; !IsBack(Item = RunMenu(Window, Renderer, Art, Sheets, Menu, Random, Cursor), Menu);)
    {
        if (*Item == MusicItem)
        {
            RunMusicMenu(Window, Renderer, Art, Sheets, Random, Settings);
        }
        else if (*Item == EffectsItem)
        {
            RunEffectsMenu(Window, Renderer, Art, Sheets, Random, Settings);
        }
        else if (*Item == QualityItem || *Item == FrequencyItem || *Item == ChannelsItem)
        {
            Sound::FOutputFormat& SoundFormat = Settings.SoundFormat;
            if (*Item == QualityItem)
            {
                SoundFormat.bEightBits = !SoundFormat.bEightBits;
            }
            else if (*Item == FrequencyItem)
            {
                SoundFormat.Rate = SoundFormat.Rate == Sound::FOutputFormat::LowRate ? Sound::FOutputFormat::HighRate : Sound::FOutputFormat::LowRate;
            }
            else
            {
                SoundFormat.bMono = !SoundFormat.bMono;
            }

            // The original reopens the sound only for Channels, so its Quality and Frequency wait for that; the port reopens it for each.
            if (GameAudio)
            {
                GameAudio->SetOutputFormat(SoundFormat);
            }
        }

        Update();
    }
}

// FUN_00425650: one page of the key settings, from the action setting at First: six of those the device lists, then [Next ...] while more follow, then Back.
// Returns the settings shown and where the next page starts.
std::pair<std::vector<size_t>, size_t> FillKeyPage(FMenu& Menu, const FSettings& Settings, const size_t First)
{
    constexpr size_t PageSize = 6;
    const std::span<const FActionSetting> All = ActionSettings();
    Menu.Items.clear();
    std::vector<size_t> Shown;
    size_t Next = First;
    for (; Next < All.size() && Shown.size() < PageSize; ++Next)
    {
        if (!ListsSetting(All[Next], EInputDevice::Keyboard))
        {
            continue;
        }

        const FBinding& Binding = Settings.Controls()[size_t(All[Next].Action)];
        Menu.Items.push_back({
            .Text = std::format("{} : {}", Localize(All[Next].Name), BindingName(Binding)),
            .Help = "Modify setting.",
        });
        Shown.push_back(Next);
    }

    while (Next < All.size() && !ListsSetting(All[Next], EInputDevice::Keyboard))
    {
        ++Next;
    }

    if (Next < All.size())
    {
        Menu.Items.push_back({ .Text = "[Next ...]", .Help = "Other settings ..." });
    }

    AddBack(Menu);
    return { Shown, Next };
}

// FUN_004253f0, the key settings: choosing one waits for a key or button and binds it alone (a fixed binding stays); [Next ...] pages on and Back pages back, then leaves.
// The original lists the device in use; the port lists the keyboard's table, every action of which takes a key or a mouse button.
void RunKeySettingsMenu(GLFWwindow* const Window, FRenderer& Renderer, const FMenuArt& Art, const FMenuSheets& Sheets, FRandom& Random, FSettings& Settings)
{
    FMenu Menu = OptionsList("/Control", "/Settings");
    std::vector<size_t> Pages{ 0 };
    auto [Shown, Next] = FillKeyPage(Menu, Settings, Pages.back());
    std::optional<std::pair<int32_t, int32_t>> Cursor;
    for (;;)
    {
        const std::optional<size_t> Item = RunMenu(Window, Renderer, Art, Sheets, Menu, Random, Cursor);
        if (!Item)
        {
            return;
        }

        if (Item == Menu.DefaultItem)
        {
            if (Pages.size() == 1)
            {
                return;
            }

            Pages.pop_back();
        }
        else if (*Item >= Shown.size())
        {
            Pages.push_back(Next);
        }
        else
        {
            const FActionSetting& Setting = ActionSettings()[Shown[*Item]];
            Menu.Items[*Item] = { .Text = std::format("{} : ", Localize(Setting.Name)), .Help = "Waiting key or button event ..." };
            // FUN_00425860 waits for every key and button to be up, then for one to go down.
            // The port binds a key pressed with Alt, Shift or Ctrl held as that combination, and a modifier alone once it is released by itself.
            bool bReleased = false;
            std::optional<uint32_t> LoneModifier;
            FBinding Binding;
            if (!ShowMenuUntil(Window, Renderer, Art, Sheets, Menu, Random, [&](uint32_t)
            {
                if (!bReleased)
                {
                    bReleased = !FirstCodeDown(Window);
                    return false;
                }

                const std::optional<uint32_t> Modifier = HeldModifier(Window);
                if (const std::optional<uint32_t> Code = FirstCodeDown(Window, false))
                {
                    Binding = { .Code = *Code, .Modifier = Modifier.value_or(0) };
                    return true;
                }

                if (Modifier)
                {
                    LoneModifier = Modifier;
                    return false;
                }

                Binding = { .Code = LoneModifier.value_or(0) };
                return LoneModifier.has_value();
            }))
            {
                return;
            }

            Settings.Bindings.BindAlone(EInputDevice::Keyboard, Setting.Action, Binding);
        }

        std::tie(Shown, Next) = FillKeyPage(Menu, Settings, Pages.back());
    }
}

// FUN_00424f30, the Control menu: the device, the direction (absolute or relative, which the mouse keeps absolute), the key settings and their defaults.
// It shows the device's picture from menu.gaf (animations 2, 3 and 7) at the screen's bottom left.
// The port plays with every device at once, so the device items do nothing and the keyboard's picture stays; the direction applies whenever the keys steer.
void RunControlMenu(GLFWwindow* const Window, FRenderer& Renderer, const FMenuArt& Art, const FMenuSheets& Sheets, FRandom& Random, FSettings& Settings)
{
    enum EItem : size_t
    {
        KeyboardItem,
        MouseItem,
        JoypadItem,
        DirectionItem,
        KeySettingsItem,
        DefaultsItem,
    };
    constexpr int32_t ControlRise = 8;
    constexpr int32_t ControlStepY = 0x28;
    FMenu Menu = OptionsMenu({}, "/Control");
    Menu.Top -= ControlRise;
    Menu.StepY = ControlStepY;
    Menu.Items = {
        { .Text = "Use Keyboard", .Help = "Select to play with Keyboard." },
        { .Text = "Use Mouse", .Help = "Select to play with Mouse." },
        { .Text = "Use JoyPad", .Help = "Select to play with Joypad." },
        {},
        { .Text = "Key settings" },
        { .Text = "Restore default settings" },
    };
    AddBack(Menu);
    const auto Update = [&]
    {
        Menu.Items[DirectionItem] = Settings.bAbsolute ? FMenuItem{ .Text = "Direction : ABSOLUTE" }
                                                       : FMenuItem{ .Text = "Direction : RELATIVE", .Help = "Turning left or right is relative to the Vyruz direction." };
        constexpr size_t KeyboardPicture = 2;
        Menu.Picture = KeyboardPicture;
    };
    Update();
    std::optional<std::pair<int32_t, int32_t>> Cursor;
    for (std::optional<size_t> Item; !IsBack(Item = RunMenu(Window, Renderer, Art, Sheets, Menu, Random, Cursor), Menu);)
    {
        if (*Item == DirectionItem)
        {
            Settings.bAbsolute = !Settings.bAbsolute;
        }
        else if (*Item == KeySettingsItem)
        {
            RunKeySettingsMenu(Window, Renderer, Art, Sheets, Random, Settings);
        }
        else if (*Item == DefaultsItem)
        {
            constexpr size_t No = 1;
            const std::optional<bool> Restore = Confirm(Window, Renderer, Art, Sheets, Random, "Restore default key settings ?", "Yes", "No", No);
            if (Restore && *Restore)
            {
                Settings.Bindings.Keyboard = FKeyBindings::DefaultTable(EInputDevice::Keyboard);
            }
        }

        Update();
    }
}

// The session folders, each a player name (FUN_0042b540 lists session\*).
std::vector<std::string> SessionNames(const FSettings& Settings)
{
    std::vector<std::string> Names;
    std::error_code Error;
    for (const std::filesystem::directory_entry& Entry : std::filesystem::directory_iterator(Settings.Folder.parent_path(), Error))
    {
        if (Entry.is_directory(Error))
        {
            Names.push_back(Entry.path().filename().string());
        }
    }

    return Names;
}

void SetPlayerName(FSettings& Settings, std::string Name)
{
    Settings.Folder = Settings.Folder.parent_path() / Name;
    Settings.PlayerName = std::move(Name);
}

// FUN_00426cd0: the names to choose one from, or to delete one with its sessions after asking.
void RunNameListMenu(GLFWwindow* const Window, FRenderer& Renderer, const FMenuArt& Art, const FMenuSheets& Sheets, FRandom& Random, FSettings& Settings,
    std::vector<std::string>& Names, const bool bDelete)
{
    constexpr size_t MaxNames = 7;
    FMenu Menu = OptionsList("/Session/Name", "/Select");
    const std::string Help = Format("Current name : %s", Settings.PlayerName);
    for (const std::string& Name : Names | std::views::take(MaxNames))
    {
        Menu.Items.push_back({ .Text = Name, .Help = Help });
    }

    if (Menu.Items.size() < MaxNames && !Names.empty())
    {
        Menu.Items.emplace_back();
    }

    AddBack(Menu, Names.empty() ? "No name to select." : "");
    std::optional<std::pair<int32_t, int32_t>> Cursor;
    for (std::optional<size_t> Item; !IsBack(Item = RunMenu(Window, Renderer, Art, Sheets, Menu, Random, Cursor), Menu);)
    {
        if (*Item >= Names.size())
        {
            continue;
        }

        if (!bDelete)
        {
            SetPlayerName(Settings, Names[*Item]);
            return;
        }

        constexpr size_t No = 1;
        const std::optional<bool> Delete = Confirm(Window, Renderer, Art, Sheets, Random, Format("Delete '%s' saved sessions ?", Names[*Item]), "Yes", "No", No);
        if (Delete && *Delete)
        {
            std::error_code Error;
            std::filesystem::remove_all(Settings.Folder.parent_path() / Names[*Item], Error);
            Names.erase(Names.begin() + std::ptrdiff_t(*Item));
            Menu.Items.erase(Menu.Items.begin() + std::ptrdiff_t(*Item));
            Menu.Items.insert(Menu.Items.begin() + std::ptrdiff_t(Names.size()), FMenuItem{});
            Menu.DefaultItem = Menu.Items.size() - 1;
        }
    }
}

// FUN_004268c0, the player names: select one, add one (up to seven) or delete one.
void RunNameMenu(GLFWwindow* const Window, FRenderer& Renderer, const FMenuArt& Art, const FMenuSheets& Sheets, FRandom& Random, FSettings& Settings)
{
    enum EItem : size_t
    {
        SelectItem,
        AddItem,
        DeleteItem,
    };
    constexpr size_t MaxNames = 7;
    std::vector<std::string> Names = SessionNames(Settings);
    FMenu Menu = OptionsMenu("/Session", "/Name");
    const auto Update = [&]
    {
        const std::string Help = Format("Current name : %s", Settings.PlayerName);
        Menu.Items = { { .Text = "Select player name", .Help = Help }, { .Text = "Add new name", .Help = Help }, { .Text = "Delete name", .Help = Help }, {}, {} };
        AddBack(Menu);
    };
    Update();
    std::optional<std::pair<int32_t, int32_t>> Cursor;
    for (std::optional<size_t> Item; !IsBack(Item = RunMenu(Window, Renderer, Art, Sheets, Menu, Random, Cursor), Menu);)
    {
        if (*Item == SelectItem || *Item == DeleteItem)
        {
            RunNameListMenu(Window, Renderer, Art, Sheets, Random, Settings, Names, *Item == DeleteItem);
            Update();
        }
        else if (*Item == AddItem && Names.size() >= MaxNames)
        {
            Menu.Items[AddItem].Help = "Cannot add more name.";
        }
        else if (*Item == AddItem)
        {
            const std::optional<std::string> Name = EditText(Window, Renderer, Art, Sheets, Menu, Random, AddItem, Localize("New name : "), "", MaxPlayerNameLength);
            if (Name && !std::ranges::contains(Names, *Name))
            {
                Names.push_back(*Name);
                SetPlayerName(Settings, *Name);
                std::error_code Error;
                std::filesystem::create_directories(Settings.Folder, Error);
            }

            Update();
        }
    }
}

// FUN_00426fa0: the four difficulty levels and their help texts.
void RunDifficultyMenu(GLFWwindow* const Window, FRenderer& Renderer, const FMenuArt& Art, const FMenuSheets& Sheets, FRandom& Random, FSettings& Settings)
{
    FMenu Menu = OptionsMenu("/Session", "/Difficulty");
    for (size_t Level = 0; Level < DifficultyLevelCount; ++Level)
    {
        Menu.Items.push_back({ .Text = std::string(DifficultyNames[Level]), .Help = std::string(DifficultyHelps[Level]) });
    }

    Menu.Items.emplace_back();
    AddBack(Menu);
    std::optional<std::pair<int32_t, int32_t>> Cursor;
    if (const std::optional<size_t> Item = RunMenu(Window, Renderer, Art, Sheets, Menu, Random, Cursor); !IsBack(Item, Menu) && *Item < DifficultyLevelCount)
    {
        Settings.DifficultyLevel = *Item;
    }
}

// FUN_00425b40, User settings: the player's name, which a running game cannot change, the difficulty and the registration key.
// The original also locks the difficulty during a game; the port lets it change and applies it as the game resumes.
// ponytail: the port needs no registration key; the item says there is none and opens nothing.
void RunUserMenu(GLFWwindow* const Window, FRenderer& Renderer, const FMenuArt& Art, const FMenuSheets& Sheets, FRandom& Random, FSettings& Settings, const bool bInGame)
{
    enum EItem : size_t
    {
        NameItem,
        DifficultyItem,
    };
    constexpr std::string_view InGameHelp = "Cannot change this setting during the game.";
    FMenu Menu = OptionsMenu({}, "/Session");
    const auto Update = [&]
    {
        Menu.Items = {
            { .Text = Format("Name : %s", Settings.PlayerName), .Help = bInGame ? std::string(InGameHelp) : "Add, delete or select player name." },
            { .Text = Format("Difficulty : %s", Localize(DifficultyNames[Settings.DifficultyLevel])), .Help = "Select to change difficulty level." },
            { .Text = "Registration key", .Help = "No key." },
            {},
            {},
        };
        AddBack(Menu);
    };
    Update();
    std::optional<std::pair<int32_t, int32_t>> Cursor;
    for (std::optional<size_t> Item; !IsBack(Item = RunMenu(Window, Renderer, Art, Sheets, Menu, Random, Cursor), Menu);)
    {
        if (*Item == NameItem && !bInGame)
        {
            RunNameMenu(Window, Renderer, Art, Sheets, Random, Settings);
        }
        else if (*Item == DifficultyItem)
        {
            RunDifficultyMenu(Window, Renderer, Art, Sheets, Random, Settings);
        }

        Update();
    }
}

// FUN_004259f0, Language: English or Russian, switched at once by LanguageSet (00401000). The texts are the archive's, so Russian needs the Russian data's Cyrillic fonts.
void RunLanguageMenu(GLFWwindow* const Window, FRenderer& Renderer, const FMenuArt& Art, const FMenuSheets& Sheets, FRandom& Random, FSettings& Settings)
{
    const std::string Help = Format("Current language is %s.", GetLanguage() == ELanguage::Russian ? Localize("Russian") : "English");
    FMenu Menu = OptionsMenu({}, "/Language");
    Menu.Items = { { .Text = "English", .Help = Help }, { .Text = "Russian", .Help = Help } };
    AddBack(Menu);
    std::optional<std::pair<int32_t, int32_t>> Cursor;
    // The port stays in English when the archive cannot draw Russian.
    if (const std::optional<size_t> Item = RunMenu(Window, Renderer, Art, Sheets, Menu, Random, Cursor);
        !IsBack(Item, Menu) && (ELanguage(*Item + 1) != ELanguage::Russian || Art.HasCyrillic()))
    {
        Settings.Language = uint32_t(*Item) + 1;
        SetLanguage(ELanguage(Settings.Language));
    }
}

// MenuOptions at 004229a0: Video, Audio (with a sound driver), Control, User settings and Language, then Back, which writes vzconf.txt (ConfigSave).
void RunOptionsMenu(GLFWwindow* const Window, FRenderer& Renderer, const FMenuArt& Art, const FMenuSheets& Sheets, FRandom& Random, FSettings& Settings, const bool bInGame)
{
    enum EItem : size_t
    {
        VideoItem,
        AudioItem,
        ControlItem,
        UserItem,
        LanguageItem,
    };
    FMenu Menu = OptionsMenu({}, {});
    Menu.Items = {
        { .Text = "Video", .Help = "Details, Device, Video Mode, Gamma." },
        { .Text = "Audio", .Help = GameAudio ? "Music, Sound FX, Quality, Frequency, Mono/Stereo." : "Sound driver (DirectSound) is unavalaible." },
        { .Text = "Control", .Help = "Input device, Key settings." },
        { .Text = "User settings", .Help = "User name, Game difficulty level." },
        { .Text = "Language", .Help = "English." },
    };
    AddBack(Menu);
    std::optional<std::pair<int32_t, int32_t>> Cursor;
    for (std::optional<size_t> Item; !IsBack(Item = RunMenu(Window, Renderer, Art, Sheets, Menu, Random, Cursor), Menu);)
    {
        if (*Item == VideoItem)
        {
            RunVideoMenu(Window, Renderer, Art, Sheets, Random, Settings);
        }
        else if (*Item == AudioItem && GameAudio)
        {
            RunAudioMenu(Window, Renderer, Art, Sheets, Random, Settings);
        }
        else if (*Item == ControlItem)
        {
            RunControlMenu(Window, Renderer, Art, Sheets, Random, Settings);
        }
        else if (*Item == UserItem)
        {
            RunUserMenu(Window, Renderer, Art, Sheets, Random, Settings, bInGame);
        }
        else if (*Item == LanguageItem)
        {
            RunLanguageMenu(Window, Renderer, Art, Sheets, Random, Settings);
        }
    }

    SaveSettings(Settings);
}

// MenuExtras at 00427820, the extras: Scores, Credits, Intro, Slide Show while vz-slide.rsc is there, and Demo, then Back, the default.
// The original's Russian build prints this menu's title and its Scores and Intro items in Russian even in English; the port keeps English there, which the English data can draw. The demo is played by the main menu's caller.
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

// MenuMain at 004271a0, the main menu. Options can change the player, whose default session the menu then offers.
// RandomNext's draws here do not matter, since every level reseeds the generator.
EMenuChoice RunMainMenu(GLFWwindow* const Window, FRenderer& Renderer, FResourceArchive& Archive, const FMenuArt& Art, std::optional<FSession>& Session,
    FSettings& Settings, const std::filesystem::path& DataFolder, FSession& Chosen)
{
    if (GameAudio)
    {
        GameAudio->CenterPans();
    }

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
    std::vector<FSession> Saved = ReadSavedSessions(Settings.Folder);
    // The count stops at the 7 sessions the list shows; the Russian text takes a second plural ending.
    const auto LoadHelp = [&]
    {
        constexpr size_t ListedSessions = 7;
        const std::string_view Plural = Saved.size() > 1 ? "s" : "";
        return Saved.empty() ? std::string("No saved session.")
                             : Format("%d saved session%s available.", std::min(Saved.size(), ListedSessions), Plural, GetLanguage() == ELanguage::Russian ? Plural : "");
    };
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

            Saved = ReadSavedSessions(Settings.Folder);
            Menu.Items[LoadItem].Help = LoadHelp();
        }
        else if (*Item == OptionsItem)
        {
            RunOptionsMenu(Window, Renderer, Art, Sheets, Random, Settings, false);
            Session = ReadSession(DefaultSessionFolder(Settings.Folder));
            Saved = ReadSavedSessions(Settings.Folder);
            Menu.Items[RestartItem] = Session ? FMenuItem{ .Text = "Restart Level", .Help = SessionHelp(*Session) } : FMenuItem{};
            Menu.Items[LoadItem].Help = LoadHelp();
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

// FUN_0041e5a0 fading the game menu's backdrop out as MenuGame closes: its background alone, in five steps from 4 down to 0, with fade.wav while the game goes on.
// With Gamma FX the ramp is (step - 4) * 255 / 4 on its own, 50 ms apart; without, the rows at or past the step in every four turn black, 25 ms apart.
void FadeMenuBackdrop(GLFWwindow* const Window, FRenderer& Renderer, const FMenuArt& Art, const FMenuSheets& Sheets, const bool bGammaFx, const bool bSound)
{
    constexpr int FadeQuarters = 4;
    constexpr uint32_t GammaFadeStepMilliseconds = 50;
    constexpr uint32_t LineFadeStepMilliseconds = 25;
    if (bSound && GameAudio)
    {
        GameAudio->PlayFade();
    }

    constexpr uint16_t White = 0x7fff;
    const FSpriteSheet Line = Renderer.CreateSpriteSheet({ Gaf::FFrame{ .Width = 1, .Height = 1, .Pixels = { White } } });
    for (int Step = FadeQuarters; Step >= 0 && !glfwWindowShouldClose(Window); --Step)
    {
        PollEvents(Window);
        if (Renderer.BeginFrame())
        {
            for (const FHudSprite& Sprite : Art.DrawBackground())
            {
                DrawHudSprite(Renderer, Sheets.Menu, Sprite);
            }

            if (bGammaFx)
            {
                const int32_t Level = (Step - FadeQuarters) * 0xff / FadeQuarters;
                Renderer.SetGamma({ Level, Level, Level });
            }
            else
            {
                constexpr uint32_t Black = 0;
                for (int Row = 0; Row < int(ScreenHeight); ++Row)
                {
                    if (Row % FadeQuarters >= Step)
                    {
                        Renderer.DrawSprite(Line, 0, 0.f, float(Row), false, false, ESpriteBlend::Opaque, float(ScreenWidth), 1.f, UINT32_MAX, Black);
                    }
                }
            }

            Renderer.EndFrame();
        }

        const uint32_t Start = ClockMilliseconds();
        while (ClockMilliseconds() - Start < (bGammaFx ? GammaFadeStepMilliseconds : LineFadeStepMilliseconds) && !glfwWindowShouldClose(Window))
        {
            PollEvents(Window);
        }
    }
}

// MenuGame at 004223e0, the menu Escape opens during a level: Resume Game, Options, Retry Level (shown while there is a default session, which a running level always has) and End Game.
std::optional<ELevelEnd> RunGameMenu(GLFWwindow* const Window, FRenderer& Renderer, const FMenuArt& Art, const FMenuSheets& Sheets, FRandom& Random, FSettings& Settings)
{
    if (GameAudio)
    {
        GameAudio->CenterPans();
    }

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
        .bStartCancels = true,
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
            RunOptionsMenu(Window, Renderer, Art, Sheets, Random, Settings, true);
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

// The first level's page names the default keys to fire and to move, each in a block of three lines (the keyboard's, the mouse's and the joypad's) that name them after " : ".
// The original prints the page as it is; the port names the keys bound now in each line whose bindings are no longer the original's defaults, and keeps the page's words otherwise.
std::string NameBoundKeys(std::string Page, const FKeyBindings& Bindings)
{
    constexpr size_t BlockLines = 3;
    constexpr std::array<EInputDevice, BlockLines> Devices{ EInputDevice::Keyboard, EInputDevice::Mouse, EInputDevice::Joypad };
    constexpr std::array FireActions{ EAction::Fire };
    constexpr std::array MoveActions{ EAction::Forward, EAction::Backward, EAction::Left, EAction::Right };
    const std::array<std::span<const EAction>, 2> BlockActions{ FireActions, MoveActions };
    constexpr std::string_view Separator = " : ";

    // Each line as its start and end, without the line break.
    std::vector<std::pair<size_t, size_t>> Lines;
    for (size_t Start = 0; Start <= Page.size();)
    {
        const size_t Break = std::min(Page.find('\n', Start), Page.size());
        const size_t End = Break > Start && Page[Break - 1] == '\r' ? Break - 1 : Break;
        Lines.emplace_back(Start, End);
        Start = Break + 1;
    }

    const auto Named = [&](const size_t Line) { return Page.substr(Lines[Line].first, Lines[Line].second - Lines[Line].first).find(Separator) != std::string::npos; };
    std::vector<size_t> Blocks;
    for (size_t Line = 0; Line + BlockLines <= Lines.size(); ++Line)
    {
        if (Named(Line) && Named(Line + 1) && Named(Line + 2) && (Line == 0 || !Named(Line - 1)) && (Line + BlockLines == Lines.size() || !Named(Line + BlockLines)))
        {
            Blocks.push_back(Line);
        }
    }

    if (Blocks.size() != BlockActions.size())
    {
        return Page;
    }

    // From the last line back, so the earlier lines' offsets hold.
    for (size_t Block = Blocks.size(); Block-- > 0;)
    {
        for (size_t Row = BlockLines; Row-- > 0;)
        {
            const FBindingTable& Table = Bindings.Table(Devices[Row]);
            const FBindingTable& Defaults = FKeyBindings::OriginalTable(Devices[Row]);
            std::string Keys;
            bool bChanged = false;
            for (const EAction Action : BlockActions[Block])
            {
                const FBinding& Binding = Table[size_t(Action)];
                bChanged |= Binding != Defaults[size_t(Action)];
                if ((Binding.Code & FixedBinding) == 0)
                {
                    Keys += (Keys.empty() ? "" : ", ") + BindingName(Binding);
                }
            }

            const size_t End = Lines[Blocks[Block] + Row].second;
            if (bChanged)
            {
                const size_t Value = Page.rfind(Separator, End) + Separator.size();
                Page.replace(Value, End - Value, Keys);
            }
        }
    }

    return Page;
}

// The screens' rows as the original showed them in cave\t1c2 with the first part of code 3 and the starting gatling, the help's first rows with the default keys,
// and v-inventory.gaf's short font record.
std::expected<std::string, std::string> VerifyInfoScreens(FResourceArchive& Archive)
{
    constexpr int32_t Top = 48;
    constexpr int32_t Rows = 9;
    const FCarriedKey Part{ .Id = 3, .bCode = true, .PartId = 1, .PartCount = 3, .CodeString = "ACTIVATE" };
    const FCarriedKey Key{ .Id = 2 };
    std::array<FInventoryWeapon, InfoWeaponSlots> Weapons;
    Weapons[0] = { .Name = "Gatling-II", .Count = 1000, .Max = 5000, .bBound = true };
    const FInfoLayout Inventory = LayoutInventory(std::array{ Part, Key }, Weapons, 0, Rows, Top);
    const auto RowText = [](const FInfoLayout& Layout, const size_t Row) { return Row < Layout.Rows.size() ? Layout.Rows[Row].Text : std::string(); };
    if (Inventory.Rows.size() != 3 || RowText(Inventory, 0) != "Code #3 : ACTIVATE-\?\?\?-\?\?\?" || RowText(Inventory, 1) != "Key #2" ||
        RowText(Inventory, 2) != "Gatling-II (1000/5000)" || Inventory.Rows[2].Y != Top + 5 * InfoRowHeight || !Inventory.bWalkedWeapons)
    {
        return std::unexpected(std::format("inventory rows {}: '{}', '{}', '{}'", Inventory.Rows.size(), RowText(Inventory, 0), RowText(Inventory, 1), RowText(Inventory, 2)));
    }

    const FInfoLayout Help = LayoutHelp(FKeyBindings::DefaultTable(EInputDevice::Keyboard), 0, Rows, Top);
    if (RowText(Help, 0) != "Help : key F1" || RowText(Help, 1) != "Panel Help : key F2" || RowText(Help, 2) != "Weapon #1 : key 1" || Help.Rows[2].Icon != EInfoIcon::Weapon)
    {
        return std::unexpected(std::format("help rows '{}', '{}', '{}'", RowText(Help, 0), RowText(Help, 1), RowText(Help, 2)));
    }

    constexpr size_t FontGlyphs = 256;
    const std::optional<std::vector<Font::FFont>> Fonts = LoadGafFonts(Archive, "common\\data\\menu\\v-inventory.gaf");
    if (!Fonts || Fonts->empty() || Fonts->front().Glyphs.size() != FontGlyphs)
    {
        return std::unexpected("v-inventory.gaf's font did not load");
    }

    return std::format("inventory and help rows as the original's, {} help rows with the default keys", Help.Count);
}

// The first level's page with the original's defaults stays as it is, a rebound key replaces only its own line's keys, and the port's modern defaults rename it.
bool VerifyBoundKeys()
{
    constexpr std::string_view Page = "Your mission : here.\r\n\r\n    Fire\t\tKeyboard : Space key\r\n\t\tMouse    : Left button\r\n\t\tJoyPad   : Button 1\r\n\r\n"
                                      "    Move\t\tKeyboard : Arrows\r\n\t\tMouse    : Right Button\r\n\t\tJoyPad   : as usual ...\r\n";
    FKeyBindings Bindings = FKeyBindings::Defaults();
    Bindings.Keyboard = FKeyBindings::OriginalTable(EInputDevice::Keyboard);
    const bool bDefaultsKept = NameBoundKeys(std::string(Page), Bindings) == Page;
    constexpr uint32_t LeftCtrl = 0x22;
    Bindings.Keyboard[size_t(EAction::Fire)] = { .Code = LeftCtrl };
    std::string Expected(Page);
    Expected.replace(Expected.find("Space key"), std::string_view("Space key").size(), CodeName(LeftCtrl));
    return bDefaultsKept && NameBoundKeys(std::string(Page), Bindings) == Expected && NameBoundKeys(std::string(Page), FKeyBindings::Defaults()) != Page;
}

// A level object by its index, which stays valid while the object list grows.
struct FObjectHandle
{
    std::vector<FLevelObject>* Objects = nullptr;
    std::optional<size_t> Index;

    FLevelObject* operator->() const { return &(*Objects)[*Index]; }
    FLevelObject& operator*() const { return (*Objects)[*Index]; }
    explicit operator bool() const { return Index.has_value(); }
};

// Command-line cheats, which the original does not have (its own are typed codes): each holds for the whole game and none touches a demo.
struct FCheats
{
    bool bGodMode = false;
    bool bInfiniteAmmo = false;
    bool bAllWeapons = false;
    bool bInfiniteMoney = false;
};

// The cash --infiniteMoney keeps the player at.
constexpr float CheatMoney = 999999.f;

// A cheat's switch for a command-line argument in any case, or null for any other argument.
bool FCheats::* CheatFlag(const std::string_view Argument)
{
    static constexpr std::pair<std::string_view, bool FCheats::*> Flags[] = {
        { "--godmode", &FCheats::bGodMode },
        { "--infiniteammo", &FCheats::bInfiniteAmmo },
        { "--allweapons", &FCheats::bAllWeapons },
        { "--allweapon", &FCheats::bAllWeapons },
        { "--infinitemoney", &FCheats::bInfiniteMoney },
    };
    std::string Lower(Argument);
    std::ranges::transform(Lower, Lower.begin(), [](const char Character) { return char(std::tolower(uint8_t(Character))); });
    const auto Found = std::ranges::find(Flags, std::string_view(Lower), &std::pair<std::string_view, bool FCheats::*>::first);
    return Found == std::end(Flags) ? nullptr : Found->second;
}

// The campaign in LEVEL-ID order (tower.floor), which the lifts follow. CAVE\T3C2 has no LEVEL-ID; T3C1's lift leads to it, so it is 3.5.
// CAVE\T2C3 (2.5) is the one level no lift leads to.
constexpr std::array<std::pair<std::string_view, std::string_view>, 24> CampaignLevels = { {
    { "1.1", "underground\\t1u1" },
    { "1.2", "cave\\t1c1" },
    { "1.3", "cave\\t1c2" },
    { "1.4", "hitek\\t1h1" },
    { "1.5", "hitek\\t1h2" },
    { "2.1", "underground\\t2u1" },
    { "2.2", "underground\\t2u2" },
    { "2.3", "cave\\t2c1" },
    { "2.4", "cave\\t2c2" },
    { "2.5", "cave\\t2c3" },
    { "2.6", "hitek\\t2h1" },
    { "2.7", "hitek\\t2h2" },
    { "2.8", "cpu\\t2p1" },
    { "3.1", "underground\\t3u1" },
    { "3.2", "underground\\t3u2" },
    { "3.3", "underground\\t3u3" },
    { "3.4", "cave\\t3c1" },
    { "3.5", "cave\\t3c2" },
    { "3.6", "cave\\t3c3" },
    { "3.7", "hitek\\t3h1" },
    { "3.8", "hitek\\t3h2" },
    { "3.9", "hitek\\t3h3" },
    { "3.10", "cpu\\t3p1" },
    { "3.11", "cpu\\t3p2" },
} };

// --level's argument as a level path: a campaign number from 1, a LEVEL-ID such as 2.3, or a path such as cave\t2c1 as it is.
std::optional<std::string> LevelArgument(const std::string_view Text)
{
    if (!Text.empty() && std::ranges::all_of(Text, [](const char Character) { return std::isdigit(uint8_t(Character)) != 0; }))
    {
        size_t Number = 0;
        std::from_chars(Text.data(), Text.data() + Text.size(), Number);
        return Number >= 1 && Number <= CampaignLevels.size() ? std::optional<std::string>(CampaignLevels[Number - 1].second) : std::nullopt;
    }

    if (Text.find_first_of("\\/") != std::string_view::npos)
    {
        return std::string(Text);
    }

    const auto Found = std::ranges::find(CampaignLevels, Text, &std::pair<std::string_view, std::string_view>::first);
    return Found == CampaignLevels.end() ? std::nullopt : std::optional<std::string>(Found->second);
}

// The cheats' hold on the player after each step: full energy and shield so nothing can kill it, every stock it holds kept full, and the cash topped up.
void ApplyCheats(const FCheats& Cheats, FPlayerInventory& Inventory)
{
    if (Cheats.bGodMode)
    {
        Inventory.Energy = Inventory.EnergyMax;
        Inventory.Shield = Inventory.ShieldMax;
    }

    if (Cheats.bInfiniteAmmo)
    {
        for (FAmmoStock& Stock : Inventory.Ammo)
        {
            Stock.Count = Stock.Count > 0.f ? Stock.Max : Stock.Count;
        }
    }

    if (Cheats.bInfiniteMoney)
    {
        Inventory.Money = CheatMoney;
    }
}

// One level of LoopLevels: runs until the window closes, the player takes a lift (the next level's path, with the player in Carried), or a menu ends or restarts the level.
// A level loaded from a session takes its objects and the player's numbers from Restore; any other becomes the default session, in Session and on disk.
std::expected<FLevelEnd, std::string> RunLevel(GLFWwindow* const Window, FRenderer& Renderer, FResourceArchive& Archive, const std::string& LevelPath,
    const std::optional<std::pair<float, float>> Start, const FCheats& Cheats, std::optional<FCarriedPlayer>& Carried, const std::span<const std::string> DebugLevels,
    FPanel* const Panel, const FMenuArt* const MenuArt, std::optional<FSession>& Session, const FSession* const Restore, FSettings& Settings,
    const std::vector<FDemoRecord>* const Demo, const bool bShowPage)
{
    // SessionLoad (0042ba00) loads the session's .3do in place of the level's, which also sets the difficulty.
    const std::optional<std::string> SessionObjects = Restore ? ReadSessionObjects(*Restore) : std::nullopt;
    if (Restore && !SessionObjects)
    {
        return std::unexpected(std::format("Cannot read the session in {}", Restore->Folder.string()));
    }

    std::expected<FLevel, std::string> Level = LoadLevel(Archive, LevelPath, SessionObjects ? std::optional<std::string_view>(*SessionObjects) : std::nullopt);
    if (!Level)
    {
        return std::unexpected(std::format("Cannot load level: {}", Level.error()));
    }

    const std::vector<std::vector<size_t>> CarriedObjects = Restore ? ResolveCarriedObjects(*Level) : std::vector<std::vector<size_t>>{};
    if (Level->DifficultyLevel && *Level->DifficultyLevel >= 0 && *Level->DifficultyLevel < int(DifficultyLevelCount))
    {
        Settings.DifficultyLevel = size_t(*Level->DifficultyLevel);
    }

    const std::optional<size_t> PlayerFound = FindPlayerObject(*Level);
    // By index: dropped objects and mines grow the object list during the level, which would leave an iterator behind.
    const FObjectHandle PlayerObject{ .Objects = &Level->Objects, .Index = PlayerFound };
    // LoopLevels puts the carried player object where the new level's player starts; a session's player keeps the level's object.
    if (Carried && !Carried->Object.ClassName.empty() && PlayerObject)
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
    // The port's wide view, which every view-bound pass follows; a demo keeps the original's, so its recorded steps meet the same objects.
    const auto LevelViewWidth = [&] { return Demo ? ScreenWidth : WideViewWidth(Window); };
    float GameViewWidth = LevelViewWidth();
    std::optional<FMovementClass> PlayerClass;
    std::optional<FPlayerState> Player;
    std::optional<FTerrainCollision> Terrain;
    const uint32_t FramePeriod = DisplayFramePeriod();
    std::optional<FCombat> Combat;
    // Session +0x101c: the last step, which the draw pass plays again.
    uint32_t LastStepMilliseconds = 0;
    std::optional<uint32_t> PlayerHitClock;
    // The smaller side of the player's current frame, which AnimUpdateBounds makes the body's bounds.
    float PlayerSpriteSize = 0.f;
    std::vector<FObjectBody> Bodies = CreateObjectBodies(*Level);
    const size_t PlayerIndex = PlayerObject.Index.value_or(Level->Objects.size());
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
    // The port's explosion light, bright in the middle and gone at its edge, and its low-shield glow, which rises toward the view's corners.
    const FSpriteSheet LightSheet = Renderer.CreateGradientSheet([](const float Distance) { return std::max(0.f, 1.f - Distance) * std::max(0.f, 1.f - Distance); });
    const FSpriteSheet EdgeSheet = Renderer.CreateGradientSheet(
        [](const float Distance)
        {
            constexpr float GlowStart = 0.75f;
            constexpr float GlowFull = 1.4f;
            const float Rise = std::clamp((Distance - GlowStart) / (GlowFull - GlowStart), 0.f, 1.f);
            return Rise * Rise;
        });
    const auto GreyLevel = [](const float Level) { return uint32_t(std::clamp(Level, 0.f, 1.f) * 255.f) * 0x010101u; };
    FMessages Messages;
    constexpr std::array<uint16_t, 4> RadarColors = { 0x7c00, 0x03e0, 0x7fe0, 0x001f };
    std::vector<Gaf::FFrame> RadarPixels;
    for (const uint16_t Color : RadarColors)
    {
        RadarPixels.push_back({ .Width = 1, .Height = 1, .ColorKey = uint16_t(~Color & 0x7fff), .Pixels = { Color } });
    }

    const FSpriteSheet ColorSheet = Renderer.CreateSpriteSheet(RadarPixels);
    FPlayerInventory Inventory = Carried ? Carried->Inventory : PlayerObject ? CreateInventory(*PlayerObject) : FPlayerInventory{};
    // A carried or saved inventory takes the class's absorptions and weight from this level's player.
    if (Carried && PlayerObject)
    {
        FPlayerInventory Class = CreateInventory(*PlayerObject);
        Inventory.Absorptions = std::move(Class.Absorptions);
        Inventory.Weight = Class.Weight;
        Inventory.ShieldUpStep = Class.ShieldUpStep;
    }
    if (PlayerObject)
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
                ApplyDifficulty(Settings.Difficulty(), *Combat, Bodies, Inventory, true);
                const FPlayerTarget Target{ .State = *Player, .Radius = PlayerClass->Radius, .Inventory = Inventory, .ObjectIndex = PlayerIndex };
                ActivateGenerators(*Level, Bodies, FPlayerTargets(&Target, 1));
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

                // A session's last.txt: the game clock, the slot in hand, the score counts and the cash; the objects the player carries become its keys and surprise.
                if (Restore)
                {
                    Combat->Clock = Restore->Clock;
                    Inventory.Money = float(Restore->Money);
                    Inventory.Earned = float(Restore->Earned);
                    Inventory.Kills = Restore->Kills;
                    Inventory.ShotsHit = Restore->ShotsHit;
                    Inventory.ShotsMissed = Restore->ShotsMissed;
                    FMessages Unused;
                    if (Restore->WeaponSlot >= 1 && Restore->WeaponSlot <= Combat->PlayerSlotStates.size())
                    {
                        SelectPlayerWeapon(*Combat, Restore->WeaponSlot, Inventory, Unused);
                    }

                    for (const size_t Index : std::views::reverse(CarriedObjects[*PlayerObject.Index]))
                    {
                        const std::optional<FPickup> Pickup = CreatePickup(*Level, Index);
                        if (Pickup && Pickup->Key)
                        {
                            Inventory.Keys.push_back(*Pickup->Key);
                        }
                        else if (Pickup && Pickup->Surprise != ESurprise::None)
                        {
                            CarrySurprise(*Combat, Inventory, Pickup->Surprise, Level->Objects[Index], Unused);
                        }
                    }
                }

                // DemoStartPlayback at 00429110 starts the game clock at the first record's time.
                if (Demo)
                {
                    Combat->Clock = Demo->front().Time;
                }

                // --allWeapons fills every stock the player class has, as the Reload Weapons surprise does, so every weapon slot is armed.
                if (Cheats.bAllWeapons && !Demo)
                {
                    for (FAmmoStock& Stock : Inventory.Ammo)
                    {
                        Stock.Count = Stock.Max;
                    }
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

                // Shots and the objects they leave behind are drawn from their classes like any object.
                std::vector<std::string> DrawnClasses;
                for (const FAmmoClass& Ammo : Combat->AmmoClasses)
                {
                    DrawnClasses.push_back(Ammo.Name);
                    for (const FCreateBlock& Create : Ammo.Creates)
                    {
                        DrawnClasses.push_back(Create.Name);
                    }
                }

                // And the trails of orbiting FX.
                for (const FEffectClass& Effect : Combat->EffectClasses)
                {
                    for (const FCreateBlock& Create : Effect.Creates)
                    {
                        DrawnClasses.push_back(Create.Name);
                    }
                }

                for (const std::string& ClassName : DrawnClasses)
                {
                    if (const auto Template = Level->ClassObjects.find(ClassName); Template != Level->ClassObjects.end())
                    {
                        for (const std::string& Path : { Template->second.GafPath, Template->second.ShadowGafPath })
                        {
                            if (!Path.empty() && !FindOrLoadSheet(*Objects, Archive, Renderer, Path))
                            {
                                std::fprintf(stderr, "Cannot decode GAF %s\n", Path.c_str());
                            }
                        }
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

    // SessionWrite (0042ad70): last.txt from the player's numbers and the level's objects as a .3do, the player's position and facing taken from its state.
    const auto CurrentSession = [&]
    {
        const FSession Written{
            .LevelPath = LevelPath,
            .LevelName = Level->Name,
            .Clock = Combat ? Combat->Clock : 0,
            .Energy = Inventory.Energy,
            .Shield = Inventory.Shield,
            .WeaponSlot = Combat ? Combat->PlayerSlot : 1,
            .DifficultyLevel = Settings.DifficultyLevel,
            .ShotsHit = Inventory.ShotsHit,
            .ShotsMissed = Inventory.ShotsMissed,
            .Kills = Inventory.Kills,
            .Earned = int(Inventory.Earned),
            .Money = int(Inventory.Money),
        };
        const FSessionPlayer Writer{
            .ObjectIndex = Player ? PlayerIndex : SIZE_MAX,
            .X = Player ? Player->Position.X : 0.f,
            .Y = Player ? Player->Position.Y : 0.f,
            .Direction = Player ? Player->Direction : 0,
            .Inventory = &Inventory,
            .State = Player ? &*Player : nullptr,
            .TurnTimeout = PlayerClass ? PlayerClass->TurnTimeout : 0,
        };
        return std::pair(Written, FormatSessionObjects(*Level, Bodies, *Doors, Writer, Settings.DifficultyLevel));
    };
    const auto WriteCurrentSession = [&](const bool bDefault, const std::pair<FSession, std::string>& Current, const std::span<const uint16_t> Thumbnail)
    {
        if (!WriteSession(Settings.Folder, bDefault, Current.first, Current.second, Thumbnail))
        {
            std::fprintf(stderr, "Cannot write a session in %s\n", Settings.Folder.string().c_str());
        }
    };

    // EngineRunLevel writes the default session as the level starts, unless the level came from a session.
    if (!Restore && !Demo)
    {
        const std::pair<FSession, std::string> Current = CurrentSession();
        WriteCurrentSession(true, Current, {});
        Session = Current.first;
        Session->Folder = DefaultSessionFolder(Settings.Folder);
    }

    // A saving plot's session waits for the frame after it, whose picture (FUN_0042b360) becomes the thumbnail.
    std::optional<std::pair<FSession, std::string>> PendingSave;
    // DAT_00522c58: the ramp Darken found, which its end puts back.
    bool bWasDark = false;
    std::optional<std::array<int32_t, 3>> DarkenRamp;

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
            .Text = Format("%s,\n   you need MORE Cash\n     to see the Map !", Settings.PlayerName),
            .X = float((int32_t(GameViewWidth) >> 1) + TextX),
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
            const FMapBox Box = LevelMapBox(*Level, GameViewWidth);
            MapSheet = Renderer.CreateSpriteSheet({ RenderLevelMap(*Level, Box) });
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
    // SessionLoad in mode 0, Load Game, starts the count on the plot the session was saved on.
    const bool bFromSavedSession = Restore && Restore->Folder.filename() != DefaultSessionFolder({}).filename();
    FSavingState Saving{ .bCounting = bFromSavedSession, .bDone = bFromSavedSession };
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
    // EngineReadActionKeys (0043b5c0) and InputActionPressed (0041d4c0) read the actions through the device's bindings (InputActionDown at 0041d650).
    const auto ActionDown = [&](const EAction Action) { return IsActionDown(Window, Settings.Controls(), Action) || IsGamepadActionDown(Action); };
    bool bEscapeDown = ActionDown(EAction::Menu);
    double CameraX = PlayerObject ? PlayerObject->X - GameViewWidth * 0.5 : 0.0;
    double CameraY = PlayerObject ? PlayerObject->Y - ScreenHeight * 0.5 : 0.0;
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
    // A lift's level change fades out and the next level fades in, the same steps as the game menu's fade, which the original does not have (its level change shows the loading screen).
    // The arrival starts black; the departure counts its own steps and ends the level once the screen is black.
    int MenuFadeStep = Carried ? 0 : MenuFadeSteps;
    uint32_t MenuFadeTime = ClockMilliseconds();
    std::optional<int> LiftFadeStep;
    int32_t FinaleTimer = 0;
    int32_t FinaleFlashTimer = 0;
    bool bFinaleFlash = false;
    // The class's Speed-max (object +0x1a4) and turn timeout, which the speed surprises and the speed toggle change.
    // A session's Turn-timeout is the player's own; high speed goes back to the class's.
    const float ClassSpeedMax = PlayerClass ? PlayerClass->SpeedMax : 0.f;
    std::string PlayerClassName = PlayerObject ? PlayerObject->ClassName : std::string();
    std::ranges::transform(PlayerClassName, PlayerClassName.begin(), [](const unsigned char Character) { return char(std::tolower(Character)); });
    const auto PlayerClassBlock = Level->Classes.find(PlayerClassName);
    const int ClassTurnTimeout = PlayerClassBlock != Level->Classes.end() ? int(ParseLevelFloat(PlayerClassBlock->second.Find("Turn-timeout")).value_or(0.f))
                                 : PlayerClass                           ? PlayerClass->TurnTimeout
                                                                         : 0;
    bool bSpeedKeyDown = ActionDown(EAction::ToggleSpeed);
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
            .bBoost = PlayerClass && PlayerClass->SpeedMax != ClassSpeedMax,
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
                const FAmmoStock* const Stock = PlayerSlotAmmo(*Combat, Inventory);
                Values.Ammo = Stock ? Stock->Count : 0.f;
            }
        }

        return Values;
    };

    FFrameClock Clock(FramePeriod);
    Clock.Poll(ClockMilliseconds());
    // Development aid: N starts the next level, wrapping to the first after the last, always without a carried player.
    bool bNextKeyDown = glfwGetKey(Window, GLFW_KEY_N) == GLFW_PRESS;
    bool bPaused = false;
    // FUN_00445580 at each level start: the level's <level>-<language>.txt, typed over the frozen level until fire or a key, then the level's name rising from the bottom.
    // Only the first level of each tower and the last level have one; a demo shows none.
    // The original shows it at every start of the level; the port only for a new game or a lift's arrival, not for a retry, a restart, a loaded game or a direct start.
    std::string PageText;
    if (!Demo && bShowPage)
    {
        if (const std::optional<std::vector<uint8_t>> File = Archive.Read(std::format("{}-{}.txt", LevelPath, TextSuffix())))
        {
            PageText = NameBoundKeys(std::string(File->begin(), File->end()), Settings.Bindings);
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
            .X = float((int32_t(GameViewWidth) >> 1) + NameX),
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
    bool bPauseKeyDown = ActionDown(EAction::Pause);
    // The forward input of the last step, which a blocked motor answers with start-engine.wav, and the surprise whose tune plays.
    bool bThrustInput = false;
    bool bFireInput = false;
    ESurprise TuneSurprise = ESurprise::None;
    // The weapon switches already heard; the level's own first selection plays nothing.
    std::optional<uint32_t> HeardWeaponSwitches;
    // Session +0x2090: the shield alarm's repeat timer.
    int32_t ShieldAlarmTimer = 0;
    // The port's hit flash, from 0 to 1, which a hit on the player raises and time fades.
    float HitFlash = 0.f;
    // The port's shake from a big blast near the player, in pixels, which time fades; it moves only the drawn view.
    float BlastShake = 0.f;
    // EngineApplyDisplayAndCursor (0042f2d0) loads Data\Menu\cursor.gaf's sight (session +0x25b8) for the mouse; EngineRunLevel plays it by the last step and draws it at the mouse after everything else.
    // The original shows it only with the mouse as the device. The port plays with every device at once: moving the mouse hands the steering to the sight, as the original's mouse device steers,
    // and a turn key hands it back to the keys. The sight shows while the mouse steers and fades once the mouse has been still a while.
    const std::optional<FObjectGaf> SightGaf = LoadObjectGaf(Archive, "common\\data\\menu\\cursor.gaf");
    const Gaf::FAnimation* const Sight = SightGaf && !SightGaf->Animations.empty() ? &SightGaf->Animations.front() : nullptr;
    FAnimationPlayback SightPlayback = Sight ? StartAnimation(*Sight) : FAnimationPlayback{};
    std::optional<FSpriteSheet> SightSheet;
    // Session +0x1f9c: only InputReadDirection (0041d7e0), which reads the live mouse, moves the sight, so a demo's replayed input leaves it where it was.
    std::optional<std::pair<int32_t, int32_t>> SightPosition;
    bool bMouseSteers = false;
    std::optional<std::pair<int32_t, int32_t>> LastMouse;
    uint32_t MouseMovedAt = 0;
    // FUN_00403b70 loads Data\Menu\pause.gaf's first animation, which FUN_00403d70 centres on the screen in draw mode 7.
    const std::optional<FObjectGaf> PauseGaf = LoadObjectGaf(Archive, "common\\data\\menu\\pause.gaf");
    // EngineRunLevel shows Data\Extra\failed.gaf's skull when the player dies: animation 2 (its dark outline, mode 8) under animation 1, centred, until the game ends (FUN_004457f0).
    const std::optional<FObjectGaf> FailedGaf = LoadObjectGaf(Archive, "common\\data\\extra\\failed.gaf");
    std::optional<FSpriteSheet> FailedSheet;
    // A white pixel that the fade without Gamma FX stretches over rows.
    std::optional<FSpriteSheet> WhiteSheet;
    FDeathDrift DeathDrift;
    // When the eased death view last moved.
    std::optional<uint32_t> DeathCameraAt;
    // The presses since the death, which end the game early.
    std::optional<FInputPress> DeathInput;
    std::optional<FSpriteSheet> PauseSheet;
    // action-next-weapon, then action-weapon-1 to 9.
    constexpr size_t WeaponKeyCount = 10;
    // g_DemoReplayCursor.
    size_t DemoCursor = 0;
    // The original replays a recording made on a 60 Hz display, one record per step; a display of another rate would run steps without records.
    // So the port plays the recording's own steps as real time allows, then 10 ms steps once they run out.
    std::vector<bool> DemoFrameEnds;
    const std::vector<uint32_t> DemoSteps = Demo ? DemoStepLengths(*Demo, &DemoFrameEnds) : std::vector<uint32_t>();
    size_t DemoStep = 0;
    uint32_t DemoBudget = 0;
    bool bTabDown = ActionDown(EAction::CyclePanels);
    bool bMapKeyDown = ActionDown(EAction::Map);
    std::array<bool, WeaponKeyCount> WeaponKeysDown{};
    bool bPreviousWeaponDown = false;
    // The pad steers once touched, until the mouse moves; while it fires, its aim holds one target.
    bool bPadSteers = false;
    std::optional<size_t> AssistTarget;
    // The aim assist pulls the aim onto the hunter in view and in clear sight closest to it within about 35 degrees,
    // and holds it there while the aim stays within about 50 degrees of it.
    constexpr float AssistCos = 0.82f;
    constexpr float AssistKeepCos = 0.64f;
    // Object +0x170, which 3do::CreateObject starts at 50; ObjectDraw counts it up to 49 for a visible object and draws it opaque from there.
    constexpr int FullFade = 49;
    int PlayerFade = FullFade + 1;
    // Session +0x2270: Time Warp's animation from Data\Object\FX\time-warp.gaf (FUN_0041b830), which ObjectDraw plays on with each drawing of the player.
    uint32_t TimeWarpAge = 0;
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
        const int Width = int(GameViewWidth);
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
    // The inventory (session +0x20d0 bit 0x200) and help (bit 0x400) screens, which EngineRunLevel runs in place of a frame (FUN_00439b00, FUN_0043ab50).
    bool bInventoryPending = false;
    bool bHelpPending = false;
    bool bInventoryKeyDown = ActionDown(EAction::Inventory);
    bool bHelpKeyDown = ActionDown(EAction::Help);
    bool bPanelHelpKeyDown = ActionDown(EAction::PanelHelp);
    // Session +0x20d0 bit 0x800, the panel help, which EngineRunLevel clears as a level starts.
    bool bPanelHelp = false;
    struct FInfoArt
    {
        // Common\Data\Menu\v-inventory.gaf (FUN_00439a20, FUN_0043b3d0): its first animation's frame is the window's tile and its first font prints the rows.
        std::vector<Gaf::FFrame> Bitmaps;
        std::vector<uint32_t> BitmapWidths;
        Gaf::FAnimationFrame Tile;
        Font::FFont Font;
        FSpriteSheet Sheet;
        // v-bonus.gaf's animations 2 and 3: the arrows that show more rows above and below.
        std::array<Gaf::FAnimation, 2> Arrows;
    };
    std::optional<FInfoArt> InfoArt;
    const auto LoadInfoArt = [&]
    {
        constexpr std::string_view InfoPath = "common\\data\\menu\\v-inventory.gaf";
        constexpr std::string_view BonusPath = "common\\data\\extra\\v-bonus.gaf";
        constexpr size_t UpArrow = 1;
        constexpr size_t DownArrow = 2;
        // FUN_00439f90 prints in draw mode 7 (PgFontSetMode at 0044ceb0) whatever the font's own.
        constexpr uint32_t InfoTextDrawMode = 7;
        auto Gaf = LoadHudGaf(Archive, InfoPath);
        const std::optional<std::vector<Font::FFont>> Fonts = LoadGafFonts(Archive, InfoPath);
        const auto Bonus = LoadHudGaf(Archive, BonusPath);
        if (!Gaf || Gaf->second.empty() || Gaf->second.front().Frames.empty() || !Fonts || Fonts->empty() || !Bonus || Bonus->second.size() <= DownArrow)
        {
            return;
        }

        Font::FFont Font = Fonts->front();
        Font.DrawMode = InfoTextDrawMode;
        std::vector<uint32_t> Widths = BitmapWidthsOf(Gaf->first);
        FSpriteSheet Sheet = Renderer.CreateSpriteSheet(Gaf->first);
        InfoArt = FInfoArt{
            .Bitmaps = std::move(Gaf->first),
            .BitmapWidths = std::move(Widths),
            .Tile = Gaf->second.front().Frames.front(),
            .Font = std::move(Font),
            .Sheet = std::move(Sheet),
            .Arrows = { Bonus->second[UpArrow], Bonus->second[DownArrow] },
        };
    };
    // The last frame shown, which the screens keep under their window (FUN_0044f6d0).
    std::optional<FSpriteSheet> BackdropSheet;
    const auto RunInfoScreen = [&](const bool bHelp, const std::optional<std::vector<uint16_t>>& Backdrop)
    {
        if (!InfoArt)
        {
            LoadInfoArt();
        }

        // FUN_00439f90 and FUN_0043afc0 close the screen without their art ("Help: Invalid black square.").
        if (!Panel || !InfoArt || InfoArt->Tile.BitmapIndex == 0 || InfoArt->Tile.BitmapIndex > InfoArt->Bitmaps.size())
        {
            return;
        }

        // FUN_00439ec0's tile: its bitmap's size. The window holds as many whole tiles as fit with one to spare, centred, and a row every 40 pixels.
        const Gaf::FAnimationFrame& Tile = InfoArt->Tile;
        const int32_t TileWidth = int32_t(InfoArt->Bitmaps[Tile.BitmapIndex - 1].Width);
        const int32_t TileHeight = int32_t(InfoArt->Bitmaps[Tile.BitmapIndex - 1].Height);
        if (TileWidth == 0 || TileHeight == 0)
        {
            return;
        }

        const int32_t ViewWidth = int32_t(ScreenWidth);
        const int32_t ViewHeight = int32_t(ScreenHeight);
        const int32_t Width = (ViewWidth - TileWidth) / TileWidth * TileWidth;
        const int32_t Height = (ViewHeight - TileHeight) / TileHeight * TileHeight;
        const int32_t Left = (ViewWidth - Width) >> 1;
        const int32_t Top = (ViewHeight - Height) >> 1;
        const int32_t VisibleRows = Height / InfoRowHeight;
        // A row's icon stands 32 pixels in and 8 down, its text 50 pixels further in.
        constexpr int32_t RowX = 0x20;
        constexpr int32_t IconDropY = 8;
        constexpr int32_t TextX = 0x32;
        const bool bBackdrop = Backdrop && Backdrop->size() == size_t(ViewWidth) * size_t(ViewHeight);
        if (bBackdrop)
        {
            if (!BackdropSheet)
            {
                BackdropSheet = Renderer.CreateVideoSheet(uint32_t(ViewWidth), uint32_t(ViewHeight));
            }

            constexpr uint32_t ChannelMask = 0x1f;
            constexpr uint32_t OpaqueAlpha = 0xff000000;
            const auto Expand = [](const uint32_t Channel) { return Channel << 3 | Channel >> 2; };
            std::vector<uint32_t> Rgba(Backdrop->size());
            std::ranges::transform(*Backdrop, Rgba.begin(), [&](const uint16_t Pixel)
            {
                return Expand(uint32_t(Pixel) >> 10 & ChannelMask) | Expand(uint32_t(Pixel) >> 5 & ChannelMask) << 8 | Expand(uint32_t(Pixel) & ChannelMask) << 16 | OpaqueAlpha;
            });
            Renderer.UpdateVideoSheet(*BackdropSheet, Rgba);
        }

        if (!WhiteSheet)
        {
            constexpr uint16_t White = 0x7fff;
            WhiteSheet = Renderer.CreateSpriteSheet({ Gaf::FFrame{ .Width = 1, .Height = 1, .Pixels = { White } } });
        }

        int32_t Scroll = 0;
        // What the last list returned, which bounds the scroll and shows the lower arrow.
        int32_t Count = 0;
        bool bOpen = true;
        const auto LayOut = [&]
        {
            if (!bOpen)
            {
                return FInfoLayout{};
            }

            if (bHelp)
            {
                return LayoutHelp(Settings.Controls(), Scroll, VisibleRows, Top);
            }

            std::array<FInventoryWeapon, InfoWeaponSlots> Weapons;
            for (size_t Slot = 1; Combat && Slot <= InfoWeaponSlots; ++Slot)
            {
                if (const FAmmoStock* const Stock = SlotAmmo(*Combat, Slot, Inventory))
                {
                    Weapons[Slot - 1] = {
                        .Name = Combat->Weapons[Combat->PlayerWeapons[Slot - 1]].ClassName,
                        .Count = int32_t(Stock->Count),
                        .Max = int32_t(Stock->Max),
                        .bBound = true,
                    };
                }
            }

            FInfoLayout Layout = LayoutInventory(Inventory.Keys, Weapons, Scroll, VisibleRows, Top);
            if (Layout.bWalkedWeapons && Combat)
            {
                ClearLastShots(*Combat);
            }

            return Layout;
        };
        // A carried item's icon is its object's animation (object +0x188), which only the screen plays now, or the still frame it was placed with.
        const auto FindPlaced = [&](const size_t ObjectIndex) { return std::ranges::find(PlacedAnimations, ObjectIndex, &FPlacedAnimation::ObjectIndex); };
        const auto ItemIcon = [&](const size_t ObjectIndex) -> std::optional<std::pair<size_t, Gaf::FAnimationFrame>>
        {
            if (const auto Placed = FindPlaced(ObjectIndex); Placed != PlacedAnimations.end())
            {
                const Gaf::FAnimation& Animation = Objects->Animations[Placed->SheetIndex][Placed->Animation];
                const uint32_t Frame = Placed->Playback.bEnded ? 0 : Placed->Playback.Frame;
                return Frame >= 1 && Frame <= Animation.Frames.size() ? std::optional(std::pair(Placed->SheetIndex, Animation.Frames[Frame - 1])) : std::nullopt;
            }

            const auto Sprite = std::ranges::find_if(Objects->Sprites, [&](const FObjectSprite& Part) { return Part.ObjectIndex == ObjectIndex && !Part.bShadow; });
            return Sprite != Objects->Sprites.end() ? std::optional(std::pair(Sprite->SheetIndex, Sprite->Frame)) : std::nullopt;
        };

        // FUN_0043a8a0 and FUN_0043b2f0: Escape, Space or Return held, or the screen's action pressed again, closes it; Up or Down held scrolls a row.
        constexpr uint32_t EscapeCode = 0x01;
        constexpr uint32_t SpaceCode = 0x20;
        constexpr uint32_t ReturnCode = 0x21;
        constexpr uint32_t UpCode = 0xc8;
        constexpr uint32_t DownCode = 0xd0;
        const EAction Action = bHelp ? EAction::Help : EAction::Inventory;
        bool bActionDown = true;
        bool bActionPressed = false;
        const auto ReadKeys = [&]
        {
            if (IsCodeDown(Window, EscapeCode) || IsCodeDown(Window, SpaceCode) || IsCodeDown(Window, ReturnCode) || std::exchange(bActionPressed, false))
            {
                bOpen = false;
                return true;
            }

            if (IsCodeDown(Window, UpCode))
            {
                Scroll -= Scroll > 0 ? 1 : 0;
                return true;
            }

            if (IsCodeDown(Window, DownCode))
            {
                Scroll += Scroll < Count - VisibleRows ? 1 : 0;
                return true;
            }

            return false;
        };

        // The keys are read 100 ms after opening, then 100 ms after each key that did something.
        constexpr int32_t RepeatMilliseconds = 100;
        int32_t RepeatTimer = RepeatMilliseconds;
        std::array<FAnimationPlayback, 2> Arrows{ StartAnimation(InfoArt->Arrows[0]), StartAnimation(InfoArt->Arrows[1]) };
        FFrameClock ScreenClock(FramePeriod);
        ScreenClock.Poll(ClockMilliseconds());
        // The frame that closes the screen still shows its window, empty.
        while (!glfwWindowShouldClose(Window))
        {
            PollEvents(Window);
            const bool bActionNow = ActionDown(Action);
            bActionPressed = bActionPressed || (bActionNow && !bActionDown);
            bActionDown = bActionNow;
            uint32_t Remaining = ScreenClock.Poll(ClockMilliseconds());
            while (Remaining != 0)
            {
                const uint32_t Before = Remaining;
                const uint32_t Step = NextGameStep(Remaining, FramePeriod, false);
                if (GameAudio)
                {
                    GameAudio->UpdateVoices();
                }

                if (RepeatTimer < 1)
                {
                    RepeatTimer = ReadKeys() ? RepeatMilliseconds : RepeatTimer;
                }
                else
                {
                    // The inventory counts down by the time still to step, the help screen by the step.
                    RepeatTimer -= int32_t(bHelp ? Step : Before);
                }

                // The update pass plays the icons of the rows below the scroll and the arrows.
                const FInfoLayout Layout = LayOut();
                Count = Layout.Count;
                for (const FInfoRow& Row : Layout.Rows)
                {
                    if (Row.Icon == EInfoIcon::Weapon)
                    {
                        Panel->StepIcon(Row.IconIndex, int32_t(Step));
                    }
                    else if (const auto Placed = Row.Icon == EInfoIcon::Item ? FindPlaced(Row.IconIndex) : PlacedAnimations.end(); Placed != PlacedAnimations.end())
                    {
                        UpdateAnimation(Placed->Playback, Objects->Animations[Placed->SheetIndex][Placed->Animation], int32_t(Step));
                    }
                }

                for (size_t Arrow = 0; Arrow < Arrows.size(); ++Arrow)
                {
                    UpdateAnimation(Arrows[Arrow], InfoArt->Arrows[Arrow], int32_t(Step));
                }
            }

            if (!Renderer.BeginFrame())
            {
                glfwWaitEventsTimeout(0.1);
                continue;
            }

            if (bBackdrop)
            {
                Renderer.DrawSprite(*BackdropSheet, 0, 0.f, 0.f);
            }

            // FUN_0043a980: the tiles in draw mode 13, whose specular 0xc8c8c8 (DAT_00516fd0) darkens what lies under each opaque pixel to 0x37/0xff of it,
            // then the window's edges in mode 10, adding blue 100 (DAT_00516fd4) along the top and left and 50 along the bottom and right.
            constexpr uint32_t TileShade = 0xc8c8c8;
            constexpr uint32_t BrightEdge = 100;
            constexpr uint32_t DimEdge = 50;
            constexpr uint32_t NoDiffuse = 0;
            for (int32_t Y = Top; Y + TileHeight <= Top + Height; Y += TileHeight)
            {
                for (int32_t X = Left; X + TileWidth <= Left + Width; X += TileWidth)
                {
                    Renderer.DrawSprite(InfoArt->Sheet, Tile.BitmapIndex - 1, float(X + Tile.OffsetX), float(Y + Tile.OffsetY), false, false, ESpriteBlend::InverseSourceColor, 1.f,
                        1.f, UINT32_MAX, NoDiffuse, TileShade);
                }
            }

            const auto Edge = [&](const int32_t X, const int32_t Y, const int32_t EdgeWidth, const int32_t EdgeHeight, const uint32_t Blue)
            {
                Renderer.DrawSprite(*WhiteSheet, 0, float(X), float(Y), false, false, ESpriteBlend::Additive, float(EdgeWidth), float(EdgeHeight), UINT32_MAX, NoDiffuse, Blue);
            };
            Edge(Left, Top, Width, 1, BrightEdge);
            Edge(Left, Top + 1, 1, Height - 1, BrightEdge);
            Edge(Left + 1, Top + Height - 1, Width - 1, 1, DimEdge);
            Edge(Left + Width - 1, Top + 1, 1, Height - 2, DimEdge);

            // The draw pass: each shown row's icon, then its text.
            const FInfoLayout Layout = LayOut();
            Count = Layout.Count;
            std::vector<FHudSprite> Glyphs;
            for (const FInfoRow& Row : Layout.Rows)
            {
                if (!Row.bDrawn)
                {
                    continue;
                }

                const float IconX = float(Left + RowX);
                const float IconY = float(Row.Y + IconDropY);
                if (const std::optional<FHudSprite> Icon = Row.Icon == EInfoIcon::Weapon ? Panel->DrawIcon(Row.IconIndex, IconX, IconY) : std::nullopt)
                {
                    DrawHudSprite(Renderer, IconSheet, *Icon);
                }
                else if (const auto Item = Row.Icon == EInfoIcon::Item ? ItemIcon(Row.IconIndex) : std::nullopt)
                {
                    DrawHudSprite(Renderer, Objects->Sheets[Item->first], { .Frame = Item->second, .X = IconX, .Y = IconY });
                }

                Glyphs.clear();
                AppendText(Glyphs, EHudSheet::Panel, InfoArt->Font, InfoArt->Bitmaps, InfoArt->BitmapWidths, Left + RowX + TextX, Row.Y, Row.Text);
                for (const FHudSprite& Glyph : Glyphs)
                {
                    DrawHudSprite(Renderer, InfoArt->Sheet, Glyph);
                }
            }

            // The arrows, 16 pixels in from the window's right and from its top or bottom.
            constexpr int32_t ArrowInset = 16;
            const auto DrawArrow = [&](const size_t Arrow, const int32_t Y)
            {
                const uint32_t Frame = Arrows[Arrow].bEnded ? 0 : Arrows[Arrow].Frame;
                if (Frame >= 1 && Frame <= InfoArt->Arrows[Arrow].Frames.size())
                {
                    DrawHudSprite(Renderer, BonusSheet, { .Frame = InfoArt->Arrows[Arrow].Frames[Frame - 1], .X = float(Left + Width - ArrowInset), .Y = float(Y) });
                }
            };
            if (Scroll > 0)
            {
                DrawArrow(0, Top + ArrowInset);
            }

            if (Scroll < Count - VisibleRows)
            {
                DrawArrow(1, Top + Height - ArrowInset);
            }

            Renderer.EndFrame();
            if (!bOpen)
            {
                break;
            }
        }
    };
    const auto Current = std::ranges::find(DebugLevels, LevelPath);
    const size_t CurrentIndex = Current == DebugLevels.end() ? DebugLevels.size() : size_t(Current - DebugLevels.begin());
    // ReadPlayerInput through InputActionPressed (0041d4c0), the live part: the keys, the mouse's sight and the port's pad. It runs once per step of a living player.
    const auto ReadPlayerIntent = [&]
    {
        // A held strafe drops the turn on its side.
        // The mouse turns toward the sight (InputReadDirection at 0041d7e0); with the absolute command the keyboard's movement keys give a direction, and the player turns toward it and thrusts once facing it.
        bool bSideLeft = ActionDown(EAction::StrafeLeft);
        bool bSideRight = ActionDown(EAction::StrafeRight);
        bool bTurnLeft = false;
        bool bTurnRight = false;
        bool bForwardKey = ActionDown(EAction::Forward);
        bool bBackwardKey = ActionDown(EAction::Backward);
        // Moving the mouse hands the steering to the sight and a turn key hands it back; a demo steers by its records.
        const std::pair<int32_t, int32_t> Mouse = ScreenCursor(Window, GameViewWidth);
        // With the absolute direction the keys alone steer, so the mouse neither turns the ship nor shows the sight.
        if (LastMouse && Mouse != *LastMouse && !Demo)
        {
            NoteMouseUsed();
            bMouseSteers = !Settings.bAbsolute;
            MouseMovedAt = ClockMilliseconds();
        }

        LastMouse = Mouse;
        if ((ActionDown(EAction::Left) && !bSideLeft) || (ActionDown(EAction::Right) && !bSideRight) || Settings.bAbsolute || Demo)
        {
            bMouseSteers = false;
        }

        if (!bMouseSteers && Settings.bAbsolute)
        {
            const FCollisionPoint Keys{
                .X = float(ActionDown(EAction::Right)) - float(ActionDown(EAction::Left)),
                .Y = float(bBackwardKey) - float(bForwardKey),
            };
            const uint32_t Input = AbsoluteKeyInput(*Player, Keys);
            bTurnLeft = (Input & PlayerInputTurnLeft) != 0 && !bSideLeft;
            bTurnRight = (Input & PlayerInputTurnRight) != 0 && !bSideRight;
            bForwardKey = (Input & PlayerInputForward) != 0;
            bBackwardKey = (Input & PlayerInputBackward) != 0;
        }
        else if (!bMouseSteers)
        {
            bTurnLeft = ActionDown(EAction::Left) && !bSideLeft;
            bTurnRight = ActionDown(EAction::Right) && !bSideRight;
        }
        else
        {
            SightPosition = Mouse;

            const FCollisionPoint ToSight{
                .X = float(Mouse.first - (TruncateLow(Player->Position.X) - int32_t(std::floor(CameraX)))),
                .Y = float(Mouse.second - (TruncateLow(Player->Position.Y) - int32_t(std::floor(CameraY)))),
            };
            const EPlayerTurn Turn = TurnTowardSight(*Player, ToSight);
            bTurnLeft = Turn == EPlayerTurn::Left && !bSideLeft;
            bTurnRight = Turn == EPlayerTurn::Right && !bSideRight;
        }

        // The pad, a deliberate addition to the original's controls, steers as a twin-stick shooter: the left stick moves by screen direction and the right stick aims.
        // Without the aim the left stick heads the ship that way as the absolute direction does; with it the ship turns to the aim and the left stick splits into thrust and strafe along the ship.
        // Firing changes nothing in the steering. A movement key or the mouse hands the steering back.
        const bool bKeysSteer = ActionDown(EAction::Forward) || ActionDown(EAction::Backward) || ActionDown(EAction::Left) || ActionDown(EAction::Right) || bSideLeft || bSideRight;
        if (TakeGamepadActivity() && !Demo)
        {
            bPadSteers = true;
            bMouseSteers = false;
        }
        else if (bKeysSteer)
        {
            bPadSteers = false;
        }

        bPadSteers = bPadSteers && !bMouseSteers && !Demo;
        float PadTurnRate = 1.f;
        if (bPadSteers)
        {
            const std::optional<std::pair<float, float>> Stick = GamepadMoveStick();
            const FCollisionPoint Way = Stick ? FCollisionPoint{ .X = Stick->first, .Y = Stick->second } : FCollisionPoint{};
            // The stick's turn eases in: slow for a slight push or near the stick's way, the class's own rate at full push and three directions off.
            const auto EasedTurnRate = [&](const FCollisionPoint Toward, const float Push)
            {
                constexpr float SlowestTurnRate = 0.2f;
                constexpr float EaseDirections = 3.f;
                const float Ease = std::clamp(AimOffDirections(*Player, Toward) / EaseDirections, 0.f, 1.f);
                return std::max(SlowestTurnRate, Push * Ease);
            };
            // The aim assist bends Axis onto the hunter in view and in clear sight closest to it within the assist cone, and holds it while Axis stays within the wider hold cone.
            const auto Assisted = [&](const FCollisionPoint Axis) -> std::optional<FCollisionPoint>
            {
                constexpr float SightRadius = 1.f;
                const float AxisLength = std::hypot(Axis.X, Axis.Y);
                const auto Along = [&](const FObjectBody& Body)
                {
                    const float DX = Body.State.Position.X - Player->Position.X;
                    const float DY = Body.State.Position.Y - Player->Position.Y;
                    const float Length = std::hypot(DX, DY);
                    return Length > 0.f && AxisLength > 0.f ? (DX * Axis.X + DY * Axis.Y) / (Length * AxisLength) : -1.f;
                };
                const auto CanAssist = [&](const FObjectBody& Body)
                {
                    const float X = Body.State.Position.X - float(std::floor(CameraX));
                    const float Y = Body.State.Position.Y - float(std::floor(CameraY));
                    return X >= 0.f && Y >= 0.f && X < GameViewWidth && Y < float(ScreenHeight) && IsAutoAimTarget(Body, *Level) &&
                           !QueryTerrainCollision(*Terrain, { .Center = Player->Center, .Radius = SightRadius }, { .Center = Body.State.Center, .Radius = SightRadius }, true);
                };
                const auto Held = AssistTarget ? std::ranges::find(Bodies, *AssistTarget, &FObjectBody::ObjectIndex) : Bodies.end();
                if (Held == Bodies.end() || Along(*Held) <= AssistKeepCos || !CanAssist(*Held))
                {
                    AssistTarget.reset();
                    float Best = AssistCos;
                    for (const FObjectBody& Body : Bodies)
                    {
                        if (const float Score = Along(Body); Score > Best && CanAssist(Body))
                        {
                            Best = Score;
                            AssistTarget = Body.ObjectIndex;
                        }
                    }
                }

                const auto Found = AssistTarget ? std::ranges::find(Bodies, *AssistTarget, &FObjectBody::ObjectIndex) : Bodies.end();
                if (Found == Bodies.end())
                {
                    return std::nullopt;
                }

                return FCollisionPoint{ .X = Found->State.Position.X - Player->Position.X, .Y = Found->State.Position.Y - Player->Position.Y };
            };
            if (const std::optional<std::pair<float, float>> AimStick = GamepadAimStick())
            {
                // By hand the facing holds until the aim is three quarters of a direction off it, so a stick resting between two directions does not twitch;
                // on a hunter it turns at the class's own rate onto the direction nearest it.
                constexpr float StickHoldDirections = 0.75f;
                const std::optional<FCollisionPoint> Target = Assisted({ .X = AimStick->first, .Y = AimStick->second });
                const FCollisionPoint Aim = Target.value_or(FCollisionPoint{ .X = AimStick->first, .Y = AimStick->second });
                PadTurnRate = EasedTurnRate(Aim, Target ? 1.f : GamepadAimPush());
                const uint32_t Input = Stick ? FacingMoveInput(*Player, Way) : 0;
                const EPlayerTurn Turn = TurnTowardAim(*Player, Aim, Target ? 0.f : StickHoldDirections);
                bForwardKey = (Input & PlayerInputForward) != 0;
                bBackwardKey = (Input & PlayerInputBackward) != 0;
                bSideLeft = (Input & PlayerInputStrafeLeft) != 0;
                bSideRight = (Input & PlayerInputStrafeRight) != 0;
                // The aim keeps turning while the ship strafes.
                bTurnLeft = Turn == EPlayerTurn::Left;
                bTurnRight = Turn == EPlayerTurn::Right;
            }
            else
            {
                // Single-stick: while the pad fires, the assist bends the heading (the stick's way, or the facing with the stick at rest), so the ship steers onto a hunter without strafing.
                const std::optional<FCollisionPoint> Target = IsGamepadActionDown(EAction::Fire) ? Assisted(Stick ? Way : PlayerFacing(*Player)) : std::nullopt;
                if (!Target)
                {
                    AssistTarget.reset();
                }

                const FCollisionPoint Heading = Target.value_or(Way);
                // Squared, so the left stick turns gently until pushed well out; it also sets the speed, which stays linear.
                PadTurnRate = Target ? EasedTurnRate(Heading, 1.f) : Stick ? EasedTurnRate(Heading, GamepadThrottle() * GamepadThrottle()) : 1.f;
                const uint32_t Input = Stick ? StickMoveInput(*Player, Heading) : 0;
                const EPlayerTurn TargetTurn = Target ? TurnTowardAim(*Player, *Target) : EPlayerTurn::None;
                bForwardKey = (Input & PlayerInputForward) != 0;
                bBackwardKey = (Input & PlayerInputBackward) != 0;
                bSideLeft = false;
                bSideRight = false;
                // On a target the turn is exact, not within the stick's one direction of slack.
                bTurnLeft = Target ? TargetTurn == EPlayerTurn::Left : (Input & PlayerInputTurnLeft) != 0;
                bTurnRight = Target ? TargetTurn == EPlayerTurn::Right : (Input & PlayerInputTurnRight) != 0;
            }
        }

        FPlayerIntent Intent{
            .bForward = bForwardKey,
            .bBackward = bBackwardKey,
            .bTurnLeft = bTurnLeft,
            .bTurnRight = bTurnRight,
            .bStrafeLeft = bSideLeft,
            .bStrafeRight = bSideRight,
            .TurnRate = PadTurnRate,
        };
        // The left stick's push sets the top speed and the strafe: thrust only pushes below a tenth of Speed-max, as the low speed relies on.
        if (bPadSteers && GamepadMoveStick())
        {
            Intent.Throttle = GamepadThrottle();
        }

        return Intent;
    };

    while (!glfwWindowShouldClose(Window))
    {
        PollEvents(Window);
        GameViewWidth = LevelViewWidth();
        Messages.ViewWidth = int32_t(GameViewWidth);
        if (Combat)
        {
            Combat->ViewWidth = GameViewWidth;
        }

        // EnginePause (0043b8d0): action-pause (P) closes an open map, or freezes the game (session +0x20 bit 0x1000000) under pause.gaf until P again; meanwhile only P is read and the clock's time is dropped.
        const bool bPausePressed = ActionDown(EAction::Pause) && !bPauseKeyDown;
        bPauseKeyDown = ActionDown(EAction::Pause);
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
            const bool bFireDown = ActionDown(EAction::Fire);
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
            // action-menu, Escape (EngineEscape at 00445b40), opens the game menu, or ends the game at once for a dead player; the menu's steps do not count as game time.
            const bool bEscapePressed = ActionDown(EAction::Menu) && !bEscapeDown;
            bEscapeDown = ActionDown(EAction::Menu);
            // A playing demo ends on Escape (EngineRunLevel checks the key before DemoReplayStep).
            if (bEscapePressed && Demo)
            {
                return FLevelEnd{ .Kind = ELevelEnd::Menu };
            }

            // The original ends the dead player's game early only on Escape; the port on any fresh press, a pad button included.
            if (DeathTimer && !DeathInput)
            {
                DeathInput.emplace(Window);
            }

            const bool bDeathSkipped = DeathTimer && DeathInput && DeathInput->Poll();
            if (bEscapePressed && bMapShown)
            {
                ToggleMap();
            }
            else if ((bEscapePressed || bDeathSkipped) && DeathTimer)
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
                const std::optional<ELevelEnd> End = RunGameMenu(Window, Renderer, *MenuArt, *MenuSheets, Combat ? Combat->Random : MenuRandom, Settings);
                // MenuGame fades its backdrop out however it closes, with fade.wav unless the game ends, and only a resumed game fades back in.
                if (End != ELevelEnd::Quit)
                {
                    FadeMenuBackdrop(Window, Renderer, *MenuArt, *MenuSheets, Settings.Details.bGammaFx, !End);
                }

                if (End)
                {
                    return FLevelEnd{ .Kind = *End };
                }

                // EngineApplyDisplayAndCursor (0042f2d0) takes what Options changed.
                if (Panel)
                {
                    Panel->SetDetails(Settings.Details);
                }

                if (Combat)
                {
                    Combat->Details = Settings.Details;
                    // Unlike the original, the difficulty can change during a game, and takes over at once.
                    ApplyDifficulty(Settings.Difficulty(), *Combat, Bodies, Inventory, false);
                }

                // The fade back in (FUN_0041e5a0) centres the pans again.
                if (GameAudio)
                {
                    GameAudio->Apply(Settings.Audio);
                    GameAudio->CenterPans();
                }

                // The mouse moved over the menu, which steers nothing.
                LastMouse.reset();

                // Then FUN_0041e5a0 fades the level in, without a sound.
                MenuFadeStep = 0;
                MenuFadeTime = ClockMilliseconds();

                bEscapeDown = ActionDown(EAction::Menu);
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

            // EngineReadActionKeys (0043b5c0): action-next-weapon, action-weapon-1 to 9 (FUN_0043bad0 skips the slot in hand), action-cycle-panels (Left Shift stepping back) and action-map.
            const bool bTabPressed = ActionDown(EAction::CyclePanels) && !bTabDown;
            bTabDown = ActionDown(EAction::CyclePanels);
            if (bTabPressed && Panel && !Demo && !bFinaleStarted)
            {
                Panel->CyclePanels(glfwGetKey(Window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS);
            }

            // action-toggle-hilo-speed (Page Down by default), FUN_0043b830: at the class's Speed-max or more the player's drops to a third of it and turns 1.5 times slower ("low speed"); otherwise both return to the class's ("high speed").
            const bool bSpeedPressed = ActionDown(EAction::ToggleSpeed) && !bSpeedKeyDown;
            bSpeedKeyDown = ActionDown(EAction::ToggleSpeed);
            if (bSpeedPressed && Player && PlayerClass && !Demo && !bFinaleStarted)
            {
                constexpr float LowSpeedScale = 1.f / 3.f;
                constexpr double LowSpeedTurnScale = 1.5f;
                constexpr uint32_t SpeedVoiceDuration = 1000;
                constexpr uint32_t SpeedVoiceHold = 2000;
                if (PlayerClass->SpeedMax >= ClassSpeedMax)
                {
                    PlayerClass->SpeedMax = ClassSpeedMax * LowSpeedScale;
                    PlayerClass->TurnTimeout = TruncateLow(double(PlayerClass->TurnTimeout) * LowSpeedTurnScale);
                    QueueVoice(EVoice::LowSpeed, 0, SpeedVoiceDuration, SpeedVoiceHold);
                }
                else
                {
                    PlayerClass->SpeedMax = ClassSpeedMax;
                    PlayerClass->TurnTimeout = ClassTurnTimeout;
                    QueueVoice(EVoice::HighSpeed, 0, SpeedVoiceDuration, SpeedVoiceHold);
                }
            }

            const bool bMapPressed = ActionDown(EAction::Map) && !bMapKeyDown;
            bMapKeyDown = ActionDown(EAction::Map);
            if (bMapPressed && Player && !Demo && !bFinaleStarted)
            {
                ToggleMap();
                QueueVoice(EVoice::Map, 0, VoiceDuration, VoiceHold);
            }

            // EngineReadActionKeys: action-help (F1) sets session +0x20d0 bit 0x400 and says Help, action-panel-help (F2) flips bit 0x800 and says Help as it sets it, and action-inventory (I) sets bit 0x200 and says Inventory.
            const bool bHelpPressed = ActionDown(EAction::Help) && !bHelpKeyDown;
            bHelpKeyDown = ActionDown(EAction::Help);
            const bool bPanelHelpPressed = ActionDown(EAction::PanelHelp) && !bPanelHelpKeyDown;
            bPanelHelpKeyDown = ActionDown(EAction::PanelHelp);
            const bool bInventoryPressed = ActionDown(EAction::Inventory) && !bInventoryKeyDown;
            bInventoryKeyDown = ActionDown(EAction::Inventory);
            if (Player && !Demo && !bFinaleStarted)
            {
                constexpr uint32_t InventoryVoiceHold = 2000;
                if (bHelpPressed)
                {
                    bHelpPending = true;
                    QueueVoice(EVoice::Help, 0, VoiceDuration, VoiceHold);
                }

                bPanelHelp = bPanelHelp != bPanelHelpPressed;
                if (bPanelHelpPressed && bPanelHelp)
                {
                    QueueVoice(EVoice::Help, 0, VoiceDuration, VoiceHold);
                }

                if (bInventoryPressed)
                {
                    bInventoryPending = true;
                    QueueVoice(EVoice::Inventory, 0, VoiceDuration, InventoryVoiceHold);
                }
            }

            std::array<bool, WeaponKeyCount> WeaponKeysNow{};
            WeaponKeysNow[0] = ActionDown(EAction::NextWeapon);
            const bool bPreviousWeaponNow = IsPreviousWeaponDown(Settings.Controls());
            for (size_t Slot = 1; Slot < WeaponKeyCount; ++Slot)
            {
                WeaponKeysNow[Slot] = ActionDown(EAction(size_t(EAction::Weapon1) + Slot - 1));
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

                // LB or the wheel steps back through the weapons, which the original cannot.
                if (bPreviousWeaponNow && !bPreviousWeaponDown)
                {
                    SelectPlayerWeapon(*Combat, PreviousPlayerWeaponSlot(*Combat, Inventory), Inventory, Messages);
                }
            }

            bPreviousWeaponDown = bPreviousWeaponNow;
            WeaponKeysDown = WeaponKeysNow;
        }

        uint32_t RemainingMilliseconds = Clock.Poll(ClockMilliseconds());
        if (bPaused || !PageText.empty() || MenuFadeStep < MenuFadeSteps || LiftFadeStep)
        {
            RemainingMilliseconds = 0;
        }

        if (DemoStep < DemoSteps.size())
        {
            DemoBudget += RemainingMilliseconds;
            RemainingMilliseconds = 0;
        }

        // The draw pass advances by the frame's last step (session +0x101c). Playing a recording, that is the last step of each recorded frame that ends this frame, so the draw pass keeps the recording machine's pace; none ending, nothing advances.
        const bool bRecordedSteps = DemoStep < DemoSteps.size();
        std::optional<uint32_t> RecordedDrawStep;
        const auto NextStep = [&]() -> std::optional<uint32_t>
        {
            if (DemoStep < DemoSteps.size())
            {
                if (DemoBudget < DemoSteps[DemoStep])
                {
                    return std::nullopt;
                }

                DemoBudget -= DemoSteps[DemoStep];
                if (DemoFrameEnds[DemoStep])
                {
                    RecordedDrawStep = DemoSteps[DemoStep];
                }

                return DemoSteps[DemoStep++];
            }

            return RemainingMilliseconds != 0 ? std::optional(NextGameStep(RemainingMilliseconds, FramePeriod, Demo != nullptr)) : std::nullopt;
        };
        // VoiceQueueUpdate (004132e0) runs at the start of the full EngineUpdateObjects, so a frame of Time Warp's player-only steps leaves the queue alone.
        size_t FrameSteps = 0;
        size_t FrameFullUpdates = 0;
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
                        // PlayerSelectWeaponAction (0043bad0) switches only to another slot.
                        if (Combat->PlayerSlot != Slot)
                        {
                            SelectPlayerWeapon(*Combat, Slot, Inventory, Messages);
                        }

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

                    const FPlayerIntent Intent = ReadPlayerIntent();
                    bThrustInput = IsThrustIntent(Intent, Inventory.Surprises, bFinaleStarted);
                    FPlayerCommand Command = PlayerCommandFromIntent(Intent, Inventory.Surprises, bFinaleStarted);
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

                    FMovementClass Movement = *PlayerClass;
                    Movement.TurnTimeout = int(float(Movement.TurnTimeout) / Intent.TurnRate);
                    if (Intent.Throttle)
                    {
                        Movement.SpeedMax *= *Intent.Throttle;
                        Command.Strafe *= *Intent.Throttle;
                    }

                    if (const FObjectBody* const Struck = StepPlayer(*Player, Movement, Command, StepMilliseconds, *Terrain, *Level, Bodies))
                    {
                        ReportDoorContact(*Doors, *Struck, *Player, Inventory, Messages);
                    }

                    if (Combat)
                    {
                        ThrowPlayerBounceSpark(*Combat, *Player, Inventory.Surprises.Active);
                    }

                    // EngineUpdateObjects puts the sound listener where the player stands (SoundSetListener at 0045edb0).
                    SetSoundListener(Player->Position.X, Player->Position.Y, PlayerObject->Altitude);
                    if (Combat)
                    {
                        SetVoiceClock(Combat->Clock);
                        StepPlayerVoices(Combat->Clock, bFireInput);
                    }
                }

                // EngineRunLevel runs the full EngineUpdateObjects only when Time Warp's counter (session +0x21f0) has run out, then sets it to the divisor (+0x21f4);
                // the steps between update the player alone, so the rest of the level moves at a quarter of its speed.
                FSurpriseState& Warp = Inventory.Surprises;
                const bool bFullUpdate = Warp.WarpCounter-- == 0;
                ++FrameSteps;
                FrameFullUpdates += bFullUpdate ? 1 : 0;
                if (bFullUpdate)
                {
                    Warp.WarpCounter = Warp.WarpDivisor;
                    const FPlayerTarget Pushed{ .State = *Player, .Radius = PlayerClass->Radius, .Inventory = Inventory, .ObjectIndex = PlayerIndex };
                    const FCollisionPoint Push = StepObjectBodies(Bodies, StepMilliseconds, *Terrain, *Level, &Pushed);
                    Player->Velocity.X = float(double(Push.X) + double(Player->Velocity.X));
                    Player->Velocity.Y = float(double(Push.Y) + double(Player->Velocity.Y));
                    Player->bPushed = Player->bPushed || Push.X != 0.f || Push.Y != 0.f;
                }

                if (!DeathTimer)
                {
                    StepShields(bFullUpdate ? std::span<FObjectBody>(Bodies) : std::span<FObjectBody>(), *Level, Inventory);
                }

                const FPlayerTarget Rider{ .State = *Player, .Radius = PlayerClass->Radius, .Inventory = Inventory, .ObjectIndex = PlayerIndex };
                if (bFullUpdate && !bFinaleStarted)
                {
                    StepActors(Combat ? &*Combat : nullptr, Bodies, StepMilliseconds, *Terrain, *Level, &Rider);
                }

                if (const FDoor* const Lift = bFullUpdate ? StepDoors(*Doors, *Level, Bodies, FPlayerTargets(&Rider, 1), StepMilliseconds, Messages) : nullptr; Lift && !NextLevel && !DeathTimer)
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
                    if (bFullUpdate && !bFinaleStarted)
                    {
                        StepGenerators(*Combat, *Level, Bodies, FPlayerTargets(&Rider, 1), StepMilliseconds);
                    }

                    constexpr uint32_t FireBit = 0x2;
                    const bool bLiveFire = Demo ? (DemoInput & FireBit) != 0 : ActionDown(EAction::Fire);
                    bFireInput = bLiveFire;
                    const bool bFire = !Inventory.Surprises.bBlockedMotor && !bFinaleStarted && bLiveFire;
                    if (!DeathTimer)
                    {
                        StepPlayerWeapon(*Combat, bFire, *Player, *PlayerObject, PlayerIndex, PlayerSpriteSize, Inventory, Messages);
                    }

                    if (bFullUpdate)
                    {
                        if (!bFinaleStarted)
                        {
                            StepTurrets(*Combat, StepMilliseconds, *Terrain, *Level, Bodies, *Player);
                        }

                        StepProjectiles(*Combat, StepMilliseconds, *Terrain, *Level, Bodies,
                            { .State = *Player, .Radius = PlayerClass->Radius, .Inventory = Inventory, .ObjectIndex = PlayerIndex, .Name = Settings.PlayerName }, Messages);
                    }

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
                                .Text = Format("Well done %s !!!\nFast Kill Bonus +%d $", Settings.PlayerName, Bonus),
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

                    // Shots and blasts still in flight during the finale leave the player unharmed.
                    if (bFinaleStarted)
                    {
                        Inventory.Energy = std::max(Inventory.Energy, EnergyBefore);
                        Inventory.Shield = std::max(Inventory.Shield, ShieldBefore);
                    }

                    // Measured before the cheats refill the player, so god mode still shows its hits. A quarter of the shield or more is the full flash.
                    if (const float Lost = (EnergyBefore - Inventory.Energy) + (ShieldBefore - Inventory.Shield); Lost > 0.f)
                    {
                        constexpr float FullFlashShare = 0.25f;
                        constexpr float MinHitFlash = 0.35f;
                        const float FullFlashLoss = std::max(1.f, Inventory.ShieldMax * FullFlashShare);
                        HitFlash = std::max(HitFlash, std::clamp(Lost / FullFlashLoss, MinHitFlash, 1.f));
                    }

                    if (!Demo)
                    {
                        ApplyCheats(Cheats, Inventory);
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

                    // Dropped items, created mines and births play their looping animation like the placed objects.
                    for (const FObjectSprite& Sprite : Objects->Sprites)
                    {
                        if (std::ranges::contains(Combat->CreatedObjects, Sprite.ObjectIndex))
                        {
                            if (std::optional<FPlacedAnimation> Placed = PlacedAnimationFor(Sprite, *Objects, *Level, ActorAnimations))
                            {
                                PlacedAnimations.push_back(std::move(*Placed));
                            }
                        }
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
                    PendingSave = CurrentSession();
                    Renderer.RequestCapture(ThumbnailWidth, ThumbnailHeight);
                }

                // The update pass of MessagesUpdate (004315f0) follows the game update.
                Messages.Update(int32_t(StepMilliseconds), 0, 0, nullptr);

                // The update pass after each step uses the view the previous frame scrolled to.
                const FViewCells View = ViewCellsAround(int(std::floor(CameraX)), int(std::floor(CameraY)), int(GameViewWidth), *Level);
                if (Combat)
                {
                    StepObjectBobs(*Combat, *Level, Bodies, StepMilliseconds, View);
                }

                AnimateActors(ActorAnimations, Bodies, *Level, *Objects, View, int32_t(StepMilliseconds));
                SyncGeneratorLights(PlacedAnimations, *Objects, Bodies);
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

            // EngineGameOver sets the user's gamma back, so the death's red does not tint the game-over image or the score table.
            Renderer.ResetGamma();
            ShowImage(Window, Renderer, Archive, "common\\data\\extra\\game-over.gaf");
            ShowFinalScore();
            return FLevelEnd{ .Kind = ELevelEnd::Menu };
        }

        if (Player)
        {
            SyncObjects();
            // The original's view follows the player's rounded position (SpriteCoordinate); the port keeps the exact one, whose whole part is that view and whose fraction shifts the drawn level, so the picture does not step a pixel at a time as the ship slows.
            const int ViewWidth = int(GameViewWidth);
            const int ViewHeight = int(ScreenHeight);
            constexpr double RoundingHalf = 0.5;
            const double TargetX = std::clamp(double(Player->Position.X) + RoundingHalf - double(ViewWidth / 2), 0., double(std::max(0, int(Level->WorldExtentX) - ViewWidth)));
            const double TargetY = std::clamp(double(Player->Position.Y) + RoundingHalf - double(ViewHeight / 2), 0., double(std::max(0, int(Level->WorldExtentY) - ViewHeight)));
            // The death drift turns square corners at full speed; the original's view snaps after it, the port's eases after it so the turns do not jar.
            const uint32_t Now = ClockMilliseconds();
            if (DeathTimer && DeathCameraAt)
            {
                constexpr double DeathCameraLagMilliseconds = 400.;
                const double Ease = 1. - std::exp(-double(Now - *DeathCameraAt) / DeathCameraLagMilliseconds);
                CameraX += (TargetX - CameraX) * Ease;
                CameraY += (TargetY - CameraY) * Ease;
            }
            else
            {
                CameraX = TargetX;
                CameraY = TargetY;
            }

            DeathCameraAt = DeathTimer ? std::optional(Now) : std::nullopt;
        }

        CameraX = std::clamp(CameraX, 0.0, std::max(0.0, double(MapWidth - GameViewWidth)));
        CameraY = std::clamp(CameraY, 0.0, std::max(0.0, double(MapHeight - ScreenHeight)));
        // EngineRunLevel moves the screen shake of a trap, Destroy-around or a blast on the player (session +0x2200 and +0x2204) one step toward still and flips its side every frame, and adds it to the view after the clamp.
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
        const uint32_t DrawStepMilliseconds = bRecordedSteps ? RecordedDrawStep.value_or(0) : LastStepMilliseconds;
        AnimateActors(ActorAnimations, Bodies, *Level, *Objects, ViewCellsAround(int(std::floor(CameraX)), int(std::floor(CameraY)), int(GameViewWidth), *Level),
            int32_t(DrawStepMilliseconds));
        SyncGeneratorLights(PlacedAnimations, *Objects, Bodies);
        StepPlacedAnimations(PlacedAnimations, *Objects, *Level, ViewCellsAround(int(std::floor(CameraX)), int(std::floor(CameraY)), int(GameViewWidth), *Level), int32_t(DrawStepMilliseconds), true);
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
            ShieldAlarmTimer -= int32_t(DrawStepMilliseconds);
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
                        .Text = std::string(Localize("DANGER : SHIELD IS LOW")),
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
                    const FSoundEmitter Emitter{
                        .X = Body.State.Position.X,
                        .Y = Body.State.Position.Y,
                        .Altitude = Level->Objects[Body.ObjectIndex].Altitude,
                        .ChannelMask = Weapon.SoundChannelMask,
                        .Priority = Weapon.SoundPriority,
                    };
                    GameAudio->UpdateObjectWeapon(Weapon, State->bSoundFiring, State->SoundTrack, State->bSoundTrigger && !Body.State.bInvisible, Stock->Count, Emitter);
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

        if (!Renderer.BeginFrame(GameViewWidth))
        {
            glfwWaitEventsTimeout(0.1);
            continue;
        }

        const int OriginX = int(std::floor(CameraX));
        const int OriginY = int(std::floor(CameraY));
        // A big blast is an explosion animation (explosion, explosion00, explosion01), as opposed to the impacts, sparks and smoke.
        const auto IsBigBlast = [&](const FEffect& Effect) { return Effect.Frame != 0 && Combat->EffectClasses[Effect.ClassIndex].Animation.Name.starts_with("explosion"); };
        // The port's blast shake, the closer the harder: a new shock wave shakes the view, as a destroyed vehicle, a mine or a grenade sends one (the shck animation),
        // and a new big blast alone, as debris or a rocket's hit makes, only nudges it.
        constexpr float ShockShake = 2.f;
        if (Combat && Player)
        {
            constexpr float ShockRange = 640.f;
            constexpr float NudgeShake = 0.5f;
            constexpr float NudgeRange = 400.f;
            for (const FEffect& Effect : Combat->Effects)
            {
                const bool bShock = Effect.Frame == 1 && Combat->EffectClasses[Effect.ClassIndex].Animation.Name == "shck";
                if (bShock || (Effect.Frame == 1 && IsBigBlast(Effect)))
                {
                    const float Distance = std::hypot(Effect.Position.X - Player->Position.X, Effect.Position.Y - Player->Position.Y);
                    const float Near = std::max(0.f, 1.f - Distance / (bShock ? ShockRange : NudgeRange));
                    BlastShake = std::max(BlastShake, (bShock ? ShockShake : NudgeShake) * Near);
                }
            }
        }

        float ShakeX = 0.f;
        float ShakeY = 0.f;
        if (BlastShake > 0.f)
        {
            // A full shock's shake fades over ShakeMilliseconds, a nudge sooner.
            constexpr float ShakeMilliseconds = 180.f;
            constexpr float ShakePeriodMilliseconds = 45.f;
            constexpr float ShakeFullDrop = ShockShake;
            constexpr float TwoPi = 6.2831853f;
            const float Phase = float(ClockMilliseconds()) / ShakePeriodMilliseconds * TwoPi;
            ShakeX = BlastShake * std::sin(Phase);
            ShakeY = BlastShake * std::cos(Phase * 1.3f) * 0.5f;
            BlastShake = std::max(0.f, BlastShake - ShakeFullDrop * float(DrawStepMilliseconds) / ShakeMilliseconds);
        }

        Renderer.SetViewShift(float(OriginX - CameraX) + ShakeX, float(OriginY - CameraY) + ShakeY);
        const uint32_t FirstX = uint32_t(OriginX) / BlockWidth;
        const uint32_t FirstY = uint32_t(OriginY) / BlockHeight;
        const uint32_t EndX = std::min(Level->GridWidth, (uint32_t(OriginX) + uint32_t(GameViewWidth) - 1) / BlockWidth + 1);
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

            // The orbiting FX, drawn like any FX object unless hidden (object flag 8).
            for (const FOrbiter& Orbiter : Combat->Orbiters)
            {
                const FEffect& Effect = Orbiter.Effect;
                const FEffectClass& Class = Combat->EffectClasses[Effect.ClassIndex];
                const auto Sheet = Objects->SheetIndices.find(Class.GafPath);
                if (Orbiter.bHidden || Effect.Frame == 0 || Sheet == Objects->SheetIndices.end())
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

            // The trails, things of one pose that play their animation over their life; Is-detail ones go with low details.
            for (const FTrail& Trail : Combat->Trails)
            {
                const auto Template = Level->ClassObjects.find(Trail.ClassName);
                if (Template == Level->ClassObjects.end() || (Template->second.bDetail && Settings.Details.bLowDetails) || Template->second.GafIndex == 0)
                {
                    continue;
                }

                const FLevelObject& Class = Template->second;
                const auto Sheet = Objects->SheetIndices.find(Class.GafPath);
                const size_t Animation = size_t(Class.GafIndex - 1);
                if (Sheet == Objects->SheetIndices.end() || Animation >= Objects->Animations[Sheet->second].size())
                {
                    continue;
                }

                const Gaf::FAnimation& Played = Objects->Animations[Sheet->second][Animation];
                const uint32_t FrameNumber = AnimationFrameAt(Played, Trail.Age);
                if (FrameNumber == 0 || Played.Frames[FrameNumber - 1].BitmapIndex == 0)
                {
                    continue;
                }

                const Gaf::FAnimationFrame& Frame = Played.Frames[FrameNumber - 1];
                const int64_t GroundY = SpriteCoordinate(Trail.Position.Y);
                Sprites.push_back({
                    .SheetIndex = Sheet->second,
                    .ObjectIndex = NoObjectIndex,
                    .Frame = Frame,
                    .X = float(SpriteCoordinate(Trail.Position.X)),
                    .Y = float(GroundY) - Trail.Altitude,
                    .Depth = SpriteDepth(GroundY, false, Class.bInGround, Class.bZShadow),
                    .Blend = FrameBlend(Frame),
                });
            }

            // ObjectDraw at 00431ae0 draws a shot as its ammo class: its shadow on the ground and its body at its altitude, in the pose of its direction and at the frame its age has reached; Invisible classes such as bullets show nothing.
            for (const FProjectile& Shot : Combat->Projectiles)
            {
                const auto Template = Level->ClassObjects.find(Combat->AmmoClasses[Shot.AmmoIndex].Name);
                if (Template == Level->ClassObjects.end() || ParseLevelFloat(Template->second.Properties.Find("Invisible")).value_or(0.f) != 0.f ||
                    Shot.State.Direction >= ObjectDirectionCount)
                {
                    continue;
                }

                const FLevelObject& Class = Template->second;
                const int64_t GroundY = SpriteCoordinate(Shot.State.Position.Y);
                for (const bool bShadow : { true, false })
                {
                    const std::string& Path = bShadow ? Class.ShadowGafPath : Class.GafPath;
                    const int BaseIndex = bShadow ? Class.ShadowIndex : Class.GafIndex;
                    const auto Sheet = Objects->SheetIndices.find(Path);
                    if (Path.empty() || BaseIndex == 0 || Sheet == Objects->SheetIndices.end())
                    {
                        continue;
                    }

                    const int PoseCount = ObjectPoseCount(Class, bShadow);
                    const size_t Animation = size_t(BaseIndex - 1 + (PoseCount > 1 ? Shot.State.Direction / (ObjectDirectionCount / PoseCount) : 0));
                    const std::vector<Gaf::FAnimation>& Animations = Objects->Animations[Sheet->second];
                    const uint32_t FrameNumber = Animation < Animations.size() ? AnimationFrameAt(Animations[Animation], Shot.Age) : 0;
                    if (FrameNumber == 0 || Animations[Animation].Frames[FrameNumber - 1].BitmapIndex == 0)
                    {
                        continue;
                    }

                    const Gaf::FAnimationFrame& Frame = Animations[Animation].Frames[FrameNumber - 1];
                    Sprites.push_back({
                        .SheetIndex = Sheet->second,
                        .ObjectIndex = NoObjectIndex,
                        .bShadow = bShadow,
                        .Frame = Frame,
                        .X = float(SpriteCoordinate(Shot.State.Position.X)),
                        .Y = float(GroundY) - (bShadow ? 0.f : float(Shot.Altitude)),
                        .Depth = SpriteDepth(GroundY, bShadow, Class.bInGround, Class.bZShadow),
                        .Blend = FrameBlend(Frame),
                    });
                }
            }

            if (!Settings.Details.bLowDetails)
            {
                AddDecorSprites(Sprites, Combat->DecorAnimations, *Objects, ViewCellsAround(OriginX, OriginY, int(GameViewWidth), *Level));
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
        // ObjectDraw at 00431ae0, for the player: the hit mark starts a red glow (session +0x2588 keeps the game clock of the hit) that fades from 125 to nothing over half a second,
        // and while the shield builds up (Shield-up-step, object +0xe4) a green glow pulses once a second up to 50, both added to the body as specular colour.
        if (Player && Combat)
        {
            constexpr uint32_t HitGlowMilliseconds = 500;
            constexpr uint32_t HitGlowShift = 2;
            constexpr double PulseRadiansPerMillisecond = double(6.2831855f) * double(0.001f);
            constexpr double PulseStrength = 50.;
            constexpr uint32_t RedShift = 16;
            constexpr uint32_t GreenShift = 8;
            if (std::exchange(Inventory.bHit, false))
            {
                PlayerHitClock = Combat->Clock;
            }

            uint32_t Red = 0;
            if (PlayerHitClock)
            {
                const uint32_t Since = Combat->Clock - *PlayerHitClock;
                Red = Since < HitGlowMilliseconds ? (HitGlowMilliseconds - Since) >> HitGlowShift : 0;
                PlayerHitClock = Since < HitGlowMilliseconds ? PlayerHitClock : std::nullopt;
            }

            const uint32_t Green = Inventory.ShieldUpStep != 0.f ? uint32_t(std::abs(int32_t(std::sin(double(Combat->Clock) * PulseRadiansPerMillisecond) * PulseStrength))) : 0;
            for (FObjectSprite& Sprite : Sprites)
            {
                if (Sprite.ObjectIndex == PlayerIndex && !Sprite.bShadow)
                {
                    Sprite.Specular = Red << RedShift | Green << GreenShift;
                }
            }
        }

        // Objects created during play (drops, births, blast parts) join the counters, an invisible one from nothing as ObjectActivate starts it.
        const size_t KnownObjects = ObjectFades.size();
        ObjectFades.resize(std::max(KnownObjects, Level->Objects.size()), FullFade + 1);
        for (const FObjectBody& Body : Bodies)
        {
            if (Body.ObjectIndex >= KnownObjects && Body.ObjectIndex < ObjectFades.size() && Body.bInvisible)
            {
                ObjectFades[Body.ObjectIndex] = 0;
            }
        }

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
                    // The light layers set their own colour (ColorReset in ObjectDraw), without the body's glow.
                    Sprite.Specular = 0;
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

        // ObjectDraw for the living player under Triple Fire: the spread (object +0x114) moves on by its step each drawn frame, within 0 to 400, and the player is drawn twice first,
        // that far out along the facing six directions round one way and then the other. The body takes draw mode 5, which DrawTexturedPolygon gives half alpha;
        // the shadow and the light layers keep their own modes.
        if (Player && !DeathTimer && Inventory.Surprises.TripleSpread != 0)
        {
            constexpr int32_t MaxSpread = 400;
            constexpr int SpreadDirections = 6;
            constexpr float GhostAlpha = 0.5f;
            FSurpriseState& Surprises = Inventory.Surprises;
            Surprises.TripleSpread = std::clamp(Surprises.TripleSpread + Surprises.TripleSpreadStep, 0, MaxSpread);
            const auto First = std::ranges::find(Sprites, PlayerIndex, &FObjectSprite::ObjectIndex);
            const auto BodySheet = Objects->SheetIndices.find(PlayerObject->GafPath);
            std::vector<FObjectSprite> Ghosts;
            for (const int Turn : { SpreadDirections, -SpreadDirections })
            {
                const FCollisionPoint Offset = WorldFacingOffset(Player->Direction + Turn, float(Surprises.TripleSpread));
                for (auto Sprite = First; Sprite != Sprites.end() && Sprite->ObjectIndex == PlayerIndex; ++Sprite)
                {
                    FObjectSprite Ghost = *Sprite;
                    Ghost.X += Offset.X;
                    Ghost.Y += Offset.Y;
                    Ghost.Alpha = !Sprite->bShadow && BodySheet != Objects->SheetIndices.end() && Sprite->SheetIndex == BodySheet->second ? GhostAlpha : 1.f;
                    Ghosts.push_back(Ghost);
                }
            }

            Sprites.insert(First, Ghosts.begin(), Ghosts.end());
        }

        // ObjectDraw for the player under Time Warp (session +0x21f4) draws time-warp.gaf's animation over it at twice the size, turned clock * 360 * 0.001 degrees, once a second.
        if (Player && !DeathTimer && Combat && Inventory.Surprises.WarpDivisor != 0)
        {
            const auto Last = std::ranges::find(Sprites | std::views::reverse, PlayerIndex, &FObjectSprite::ObjectIndex);
            const std::optional<size_t> Sheet = FindOrLoadSheet(*Objects, Archive, Renderer, "common\\data\\object\\fx\\time-warp.gaf");
            if (Last != Sprites.rend() && Sheet && !Objects->Animations[*Sheet].empty())
            {
                TimeWarpAge += LastStepMilliseconds;
                const Gaf::FAnimation& Played = Objects->Animations[*Sheet].front();
                const uint32_t FrameNumber = AnimationFrameAt(Played, TimeWarpAge);
                constexpr double DegreesPerTurn = double(360.f);
                constexpr double TurnsPerMillisecond = double(0.001f);
                constexpr int64_t FullTurnDegrees = 360;
                constexpr float WarpStretch = 2.f;
                const int64_t Degrees = int64_t(double(Combat->Clock) * DegreesPerTurn * TurnsPerMillisecond) % FullTurnDegrees;
                if (FrameNumber != 0)
                {
                    const Gaf::FAnimationFrame& Frame = Played.Frames[FrameNumber - 1];
                    FObjectSprite Warp = *Last;
                    Warp.SheetIndex = *Sheet;
                    Warp.Frame = Frame;
                    Warp.bShadow = false;
                    Warp.Blend = FrameBlend(Frame);
                    Warp.Stretch = WarpStretch;
                    Warp.Diffuse = 0xffffff;
                    Warp.Specular = 0;
                    Warp.Alpha = 1.f;
                    Warp.Rotation = float(Degrees);
                    Sprites.insert(Last.base(), Warp);
                }
            }
        }
        else
        {
            TimeWarpAge = 0;
        }

        // The port's outlines of the player and the live hunters where the terrain covers them, each object's tint or none; a fading or invisible object keeps none, so it stays hidden.
        constexpr uint32_t PlayerOutline = 0x5ad2ff;
        constexpr uint32_t EnemyOutline = 0xff5a46;
        std::vector<uint32_t> OutlineTints(Level->Objects.size(), 0);
        if (Renderer.CanOutline())
        {
            if (Player && PlayerIndex < OutlineTints.size() && PlayerFade >= FullFade)
            {
                OutlineTints[PlayerIndex] = PlayerOutline;
            }

            for (const FObjectBody& Body : Bodies)
            {
                if (Body.ObjectIndex < OutlineTints.size() && IsAutoAimTarget(Body, *Level) && (Body.ObjectIndex >= ObjectFades.size() || ObjectFades[Body.ObjectIndex] >= FullFade))
                {
                    OutlineTints[Body.ObjectIndex] = EnemyOutline;
                }
            }
        }

        // Only an outlined object's opaque body parts, not its shadow or its additive glows and lights.
        const auto IsOutlined = [&](const FObjectSprite& Sprite)
        { return !Sprite.bShadow && Sprite.Blend == ESpriteBlend::Opaque && Sprite.ObjectIndex < OutlineTints.size() && OutlineTints[Sprite.ObjectIndex] != 0; };
        bool bRedrawUncovered = false;
        const auto DrawObject = [&](const FObjectSprite& Sprite, const uint32_t OutlineTint = 0)
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
            // The player's own sprites sit at its exact position, so it holds still against the smoothly following view.
            const float SubX = Player && Sprite.ObjectIndex == PlayerIndex ? Player->Position.X - float(SpriteCoordinate(Player->Position.X)) : 0.f;
            const float SubY = Player && Sprite.ObjectIndex == PlayerIndex ? Player->Position.Y - float(SpriteCoordinate(Player->Position.Y)) : 0.f;
            const float X = Sprite.X + SubX - float(OriginX);
            const float Y = Sprite.Y + SubY - float(OriginY);
            const int ObjectFade = Sprite.ObjectIndex < ObjectFades.size() && Sprite.ObjectIndex != PlayerIndex ? ObjectFades[Sprite.ObjectIndex] : FullFade;
            if (ObjectFade == 0)
            {
                return;
            }

            // ObjectDraw drops a fading object's shadow, lights and overlays and draws only its body by alpha; a shadow by alpha would show as a light blob.
            const bool bFadingObject = ObjectFade < FullFade;
            if ((bFadingPlayer || bFadingObject) && (Sprite.bShadow || Sprite.bGeneratorLight))
            {
                return;
            }

            // DrawAnimationFrame at 0045eab0: a frame's event flag 4 sets the diffuse colour to the event value, scaled by the colour the draw passes.
            // The player's light layers fade by pose this way.
            constexpr uint32_t DiffuseEventFlag = 4;
            constexpr uint32_t ColorMask = 0xffffff;
            const uint32_t Diffuse = (Frame.EventFlags & DiffuseEventFlag) != 0 ? ModulateColor(uint32_t(Frame.EventValue) & ColorMask, Sprite.Diffuse) : Sprite.Diffuse;
            if (OutlineTint != 0)
            {
                Renderer.DrawSprite(Objects->Sheets[Sprite.SheetIndex], Frame.BitmapIndex - 1, X + OffsetX, Y + OffsetY, Frame.bMirrorX, Frame.bMirrorY, ESpriteBlend::Alpha,
                    Scale.X, Scale.Y, UINT32_MAX, OutlineTint, 0, 1.f, Sprite.Rotation, X, Y);
                return;
            }

            // An outlined object's body unmarks the stencil under it, so only the terrain drawn after it marks what covers it.
            const bool bOutlined = IsOutlined(Sprite) && !bRedrawUncovered;
            if (bOutlined)
            {
                Renderer.SetStencilMode(EStencilMode::ClearEntity);
            }

            Renderer.DrawSprite(Objects->Sheets[Sprite.SheetIndex], Frame.BitmapIndex - 1, X + OffsetX, Y + OffsetY,
                Frame.bMirrorX, Frame.bMirrorY, bFadingPlayer || bFadingObject || Sprite.Alpha < 1.f ? ESpriteBlend::Alpha : Sprite.Blend, Scale.X, Scale.Y, UINT32_MAX, Diffuse,
                Sprite.Specular, bFadingPlayer ? PlayerAlpha : bFadingObject ? float(ObjectFade) * FadeAlphaStep : Sprite.Alpha, Sprite.Rotation, X, Y);
            if (bOutlined)
            {
                Renderer.SetStencilMode(EStencilMode::Off);
            }
        };
        const auto DrawMapRow = [&](const int64_t Row, const std::optional<int32_t> Depth)
        {
            if (Row < FirstY || Row >= EndY)
            {
                return;
            }

            // LevelDrawTerrainRow at 0041fd40 leaves the first two planes in the background cache.
            Renderer.SetStencilMode(EStencilMode::MarkTerrain);
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

            Renderer.SetStencilMode(EStencilMode::Off);
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
        // The port's blast light: a warm glow on the ground under each big blast, fading with its animation; walls and bodies drawn after it stay unlit.
        if (Combat)
        {
            constexpr float LightRadius = 110.f;
            constexpr float GroundFlatten = 0.6f;
            constexpr float LightPeak = 0.55f;
            constexpr uint32_t LightColor = 0xffc080;
            const float SheetSize = float(LightSheet.Frames.front().Width);
            for (const FEffect& Effect : Combat->Effects)
            {
                const FEffectClass& Class = Combat->EffectClasses[Effect.ClassIndex];
                if (!IsBigBlast(Effect) || Class.Animation.Frames.empty())
                {
                    continue;
                }

                const float Left = 1.f - float(Effect.Frame - 1) / float(Class.Animation.Frames.size());
                const float Radius = LightRadius * (Class.Stretch > 0.f ? Class.Stretch : 1.f);
                Renderer.DrawSprite(LightSheet, 0, Effect.Position.X - float(OriginX) - Radius, Effect.Position.Y - float(OriginY) - Radius * GroundFlatten, false, false,
                    ESpriteBlend::Screen, 2.f * Radius / SheetSize, 2.f * Radius * GroundFlatten / SheetSize, UINT32_MAX, ModulateColor(LightColor, GreyLevel(Left * Left * LightPeak)));
            }
        }

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
        // The port redraws an outlined player's body only where no terrain covers it, and leaves its glows as the level drew them in the lit window around it,
        // so the player no longer shows through the terrain in front of it.
        if (Player && Inventory.Surprises.Active == ESurprise::Darken)
        {
            DrawDarkness(*Player, OriginX, OriginY);
            const bool bPlayerOutlined = PlayerIndex < OutlineTints.size() && OutlineTints[PlayerIndex] != 0;
            for (const FObjectSprite& Sprite : Sprites)
            {
                if (Sprite.bShadow)
                {
                    continue;
                }

                if (Sprite.ObjectIndex == PlayerIndex && bPlayerOutlined)
                {
                    if (IsOutlined(Sprite))
                    {
                        bRedrawUncovered = true;
                        Renderer.SetStencilMode(EStencilMode::Uncovered);
                        DrawObject(Sprite);
                        Renderer.SetStencilMode(EStencilMode::Off);
                        bRedrawUncovered = false;
                    }
                }
                else if (Sprite.ObjectIndex == PlayerIndex || (Sprite.ObjectIndex < LightObjects.size() && LightObjects[Sprite.ObjectIndex]))
                {
                    DrawObject(Sprite);
                }
            }
        }

        // The port's outlines over the drawn level: each outlined object again, as a subtle edge and fill, only where the terrain marked it.
        if (std::ranges::any_of(OutlineTints, [](const uint32_t Tint) { return Tint != 0; }))
        {
            Renderer.SetStencilMode(EStencilMode::Outline);
            for (const FObjectSprite& Sprite : Sprites)
            {
                if (IsOutlined(Sprite))
                {
                    DrawObject(Sprite, OutlineTints[Sprite.ObjectIndex]);
                }
            }

            Renderer.SetStencilMode(EStencilMode::Off);
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

        // The port's bloom glows around the level's lights, under the messages and the panel.
        Renderer.Bloom();
        if (HitFlash > 0.f)
        {
            constexpr float HitFlashMilliseconds = 250.f;
            Renderer.Aberrate(HitFlash);
            HitFlash = std::max(0.f, HitFlash - float(DrawStepMilliseconds) / HitFlashMilliseconds);
        }

        Renderer.SetViewShift(0.f, 0.f);
        // The port's low-shield warning: a red glow pulses at the view's edges while the shield is nearly gone.
        if (Player && !DeathTimer && Inventory.Energy > 0.f && Inventory.ShieldMax > 0.f)
        {
            constexpr float LowShieldRatio = 0.2f;
            constexpr uint32_t PulseMilliseconds = 900;
            constexpr float EdgeLow = 0.15f;
            constexpr float EdgeHigh = 0.45f;
            constexpr uint32_t EdgeColor = 0xff2010;
            constexpr float TwoPi = 6.2831853f;
            if (Inventory.Shield / Inventory.ShieldMax <= LowShieldRatio)
            {
                const float Pulse = 0.5f - 0.5f * std::cos(float(ClockMilliseconds() % PulseMilliseconds) / float(PulseMilliseconds) * TwoPi);
                const float SheetSize = float(EdgeSheet.Frames.front().Width);
                Renderer.DrawSprite(EdgeSheet, 0, 0.f, 0.f, false, false, ESpriteBlend::Screen, GameViewWidth / SheetSize, ScreenHeight / SheetSize, UINT32_MAX,
                    ModulateColor(EdgeColor, GreyLevel(EdgeLow + (EdgeHigh - EdgeLow) * Pulse)));
            }
        }

        // The messages' draw pass comes after the objects and before the panel, with the last step; the frame that opens the inventory or the help skips it.
        if (Panel && !bInventoryPending && !bHelpPending)
        {
            std::vector<FHudSprite> MessageSprites;
            const FMessages::FDraw DrawMessage = [&](const FMessage& Message, const int32_t X, const int32_t Y, const std::string_view Text)
            {
                Panel->DrawMessage(MessageSprites, Message, X, Y, Text);
            };
            Messages.Update(int32_t(DrawStepMilliseconds), OriginX, OriginY, &DrawMessage);
            DrawHud(MessageSprites);
        }

        if (Panel && !PageText.empty())
        {
            DrawHud(Panel->DrawShade(int(GameViewWidth), int(ScreenHeight)));
        }

        if (Panel && Player)
        {
            const std::vector<FRadarDot> Radar = FindRadarDots(*Level, Bodies, Player->Position, PlayerIndex, int(ScreenHeight));
            DrawHud(Panel->Draw(PanelValues(), Radar, int(GameViewWidth), int(ScreenHeight), Combat ? Combat->MiscRandom : NoMiscRandom));
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

        if (GameAudio && (FrameSteps == 0 || FrameFullUpdates != 0))
        {
            GameAudio->UpdateVoices();
        }

        // PanelDrawBigRadar (004362a0) after the panel: the map, then icons scaled from the level's size, the player's (animation 21) and every lift's (23, lift) and saving plot's (25, saveuk), each list from its last.
        if (bMapShown && MapSheet && Panel && Player)
        {
            constexpr size_t PlayerIcon = 21;
            constexpr size_t LiftIcon = 23;
            constexpr size_t SavingIcon = 25;
            // The game behind the map blurs and darkens; a picture made for another view width is scaled into this one's box.
            constexpr uint32_t MapBackdropTint = 0x707078;
            Renderer.BlurView(MapBackdropTint);
            const FMapBox Box = LevelMapBox(*Level, GameViewWidth);
            const FSpriteFrame& Picture = MapSheet->Frames.front();
            const float PictureScale = Picture.Width == 0 ? 1.f : Box.Width / float(Picture.Width);
            Renderer.DrawSprite(*MapSheet, 0, Box.Left, Box.Top, false, false, ESpriteBlend::Opaque, PictureScale, PictureScale);
            const double ScaleX = Box.WorldWidth > 0.f ? double(Box.Width) / double(Box.WorldWidth) : 0.;
            const double ScaleY = Box.WorldHeight > 0.f ? double(Box.Height) / double(Box.WorldHeight) : 0.;
            const auto Icon = [&](const size_t Animation, const float X, const float Y)
            {
                const float IconX = Box.Left + float(TruncateLow(double(X - Box.WorldLeft) * ScaleX));
                const float IconY = Box.Top + float(TruncateLow(double(Y - Box.WorldTop) * ScaleY));
                if (const std::optional<FHudSprite> Sprite = Panel->DrawAnimation(Animation, IconX, IconY))
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
            DrawHud(Panel->DrawTypedPage(PageFont, PageText, PageTyped, int(GameViewWidth), int(ScreenHeight)));
        }

        if (DeathTimer && FailedGaf && FailedGaf->Animations.size() >= 2)
        {
            if (!FailedSheet)
            {
                FailedSheet = Renderer.CreateSpriteSheet(FailedGaf->Bitmaps);
            }

            const float CentreX = float(int32_t(GameViewWidth) >> 1);
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

            UpdateAnimation(SightPlayback, *Sight, int32_t(DrawStepMilliseconds));
            const uint32_t Frame = SightPlayback.bEnded ? 0 : SightPlayback.Frame;
            // Full while the mouse moves, then fading out once it has been still for a while.
            constexpr uint32_t SightIdleMilliseconds = 3000;
            constexpr uint32_t SightFadeMilliseconds = 500;
            const uint32_t Still = ClockMilliseconds() - MouseMovedAt;
            const float Shown = !bMouseSteers || Still >= SightIdleMilliseconds + SightFadeMilliseconds ? 0.f
                              : Still <= SightIdleMilliseconds                                         ? 1.f
                                                                                                       : 1.f - float(Still - SightIdleMilliseconds) / float(SightFadeMilliseconds);
            if (Frame >= 1 && Frame <= Sight->Frames.size() && SightPosition && Shown > 0.f)
            {
                // An additive sight fades by darkening; any other by its alpha.
                const Gaf::FAnimationFrame& SightFrame = Sight->Frames[Frame - 1];
                const bool bAdditive = FrameBlend(SightFrame) == ESpriteBlend::Additive;
                constexpr float FullChannel = 255.f;
                const uint32_t Channel = uint32_t(FullChannel * Shown);
                DrawHudSprite(Renderer, *SightSheet, {
                    .Frame = SightFrame,
                    .X = float(SightPosition->first),
                    .Y = float(SightPosition->second),
                    .Diffuse = bAdditive ? Channel << 16 | Channel << 8 | Channel : 0xffffff,
                    .Alpha = bAdditive ? 1.f : Shown,
                });
            }
        }

        // EngineRunLevel draws the panel help last.
        if (bPanelHelp && Panel)
        {
            DrawHud(Panel->DrawHelp(int(GameViewWidth), int(ScreenHeight)));
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
                const float X = float((int32_t(GameViewWidth) - int32_t(Bitmap.Width)) / 2 - Bitmap.TrimX);
                const float Y = float((int32_t(ScreenHeight) - int32_t(Bitmap.Height)) / 2 - Bitmap.TrimY);
                Renderer.DrawSprite(*PauseSheet, Frame.BitmapIndex - 1, X, Y, false, false, FrameBlend(Frame));
            }
        }

        // The level's fade in after the game menu: each step darkens by a quarter less, the ramp (step - 4) * 255 / 4 on its own, or without Gamma FX the rows at or past the step in every four turned black.
        // MenuGame then gives the ramp the user's gamma back (FUN_00420990). A lift's departure runs the same steps the other way, a quarter darker each.
        if (MenuFadeStep < MenuFadeSteps || LiftFadeStep)
        {
            constexpr int FadeQuarters = 4;
            const int DarkQuarters = LiftFadeStep ? std::min(*LiftFadeStep + 1, FadeQuarters) : FadeQuarters - MenuFadeStep;
            if (Settings.Details.bGammaFx)
            {
                const int32_t Ramp = -DarkQuarters * 0xff / FadeQuarters;
                Renderer.SetGamma({ Ramp, Ramp, Ramp });
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
                    if (Row % FadeQuarters >= FadeQuarters - DarkQuarters)
                    {
                        Renderer.DrawSprite(*WhiteSheet, 0, 0.f, float(Row), false, false, ESpriteBlend::Opaque, GameViewWidth, 1.f, UINT32_MAX, Black);
                    }
                }
            }

            // FUN_00469780 waits 50 ms between steps with Gamma FX and 25 without.
            constexpr uint32_t GammaFadeStepMilliseconds = 50;
            constexpr uint32_t LineFadeStepMilliseconds = 25;
            const uint32_t FadeStepMilliseconds = Settings.Details.bGammaFx ? GammaFadeStepMilliseconds : LineFadeStepMilliseconds;
            if (!LiftFadeStep && ClockMilliseconds() - MenuFadeTime >= FadeStepMilliseconds)
            {
                MenuFadeTime = ClockMilliseconds();
                if (++MenuFadeStep == MenuFadeSteps)
                {
                    Renderer.ResetGamma();
                }
            }
        }

        // SurpriseStart (0041c390) with Gamma FX keeps the ramp and sets the user's gamma as Darken starts; SurpriseEnd (0041cd10) puts the kept ramp back.
        if (const bool bDark = Inventory.Surprises.Active == ESurprise::Darken; bDark != bWasDark && Settings.Details.bGammaFx)
        {
            if (bDark)
            {
                DarkenRamp = Renderer.GetGamma();
                Renderer.SetGamma(Renderer.GetUserGamma());
            }
            else if (DarkenRamp)
            {
                Renderer.SetGamma(*DarkenRamp);
            }
        }

        bWasDark = Inventory.Surprises.Active == ESurprise::Darken;
        // During the finale (FUN_00445dd0) the ramp goes white for each flash and back to the user's gamma; for the last 2.5 seconds FUN_00446060 fades it from white to black along ln(e - t (e - 1) / 2500).
        if (Settings.Details.bGammaFx && FinaleTimer > 0)
        {
            if (FinaleTimer >= FinaleFadeMilliseconds)
            {
                constexpr int32_t White = 0xff;
                Renderer.SetGamma(bFinaleFlash ? std::array{ White, White, White } : Renderer.GetUserGamma());
                bFinaleFlash = false;
            }
            else
            {
                constexpr double E = 2.718281828459045;
                constexpr double FadeRate = 0.0004;
                constexpr double FadeSpan = 0x1fe;
                constexpr int32_t FadeOffset = 0xff;
                const double Elapsed = double(FinaleFadeMilliseconds - FinaleTimer);
                const int32_t Ramp = TruncateLow(FadeSpan * std::log(E - Elapsed * (E - 1.) * FadeRate)) - FadeOffset;
                Renderer.SetGamma({ Ramp, Ramp, Ramp });
            }
        }
        // EngineRunLevel with Gamma FX, while no lift takes the player and on a frame with an offset set: a living player's ramp is the user's gamma plus the offsets, which then step one toward zero,
        // and once all three are zero the ramp is the user's gamma again; Darken leaves both alone. A dead player's ramp is the offsets alone, which stay.
        else if (std::array<int32_t, 3>& Gamma = Inventory.Surprises.Gamma;
                 Settings.Details.bGammaFx && !NextLevel && MenuFadeStep == MenuFadeSteps && std::ranges::any_of(Gamma, [](const int32_t Channel) { return Channel != 0; }))
        {
            if (Inventory.Energy <= 0.f)
            {
                Renderer.SetGamma(Gamma);
            }
            else if (Inventory.Surprises.Active != ESurprise::Darken)
            {
                const std::array<int32_t, 3>& User = Renderer.GetUserGamma();
                Renderer.SetGamma({ Gamma[0] + User[0], Gamma[1] + User[1], Gamma[2] + User[2] });
                for (int32_t& Channel : Gamma)
                {
                    Channel -= Channel > 0 ? 1 : Channel < 0 ? -1 : 0;
                }

                if (std::ranges::all_of(Gamma, [](const int32_t Channel) { return Channel == 0; }))
                {
                    Renderer.ResetGamma();
                }
            }
        }

        // The next frame shows an opened screen over this one; a save's thumbnail takes the copy first.
        const bool bScreenCapture = (bInventoryPending || bHelpPending) && !PendingSave;
        if (bScreenCapture)
        {
            Renderer.RequestCapture(uint32_t(ScreenWidth), uint32_t(ScreenHeight));
        }

        Renderer.EndFrame();
        if (PendingSave)
        {
            const std::optional<std::vector<uint16_t>> Thumbnail = Renderer.TakeCapture();
            WriteCurrentSession(false, *PendingSave, Thumbnail ? std::span<const uint16_t>(*Thumbnail) : std::span<const uint16_t>());
            PendingSave.reset();
        }

        if (bScreenCapture)
        {
            // EngineRunLevel takes the inventory first; its loop leaves the bit set until it closes.
            const std::optional<std::vector<uint16_t>> Backdrop = Renderer.TakeCapture();
            if (std::exchange(bInventoryPending, false))
            {
                RunInfoScreen(false, Backdrop);
            }

            if (std::exchange(bHelpPending, false))
            {
                RunInfoScreen(true, Backdrop);
            }

            // PgPoll(0x1e000) as a screen closes: its time is not game time, and the keys it took start over.
            bEscapeDown = ActionDown(EAction::Menu);
            bHelpKeyDown = ActionDown(EAction::Help);
            bInventoryKeyDown = ActionDown(EAction::Inventory);
            Clock = FFrameClock(FramePeriod);
            Clock.Poll(ClockMilliseconds());
        }

        // EngineRunLevel ends the game after a starred lift: the registered game's two ending pages over theend.tga (FUN_00402890), then the score table and the menu.
        if (NextLevel && NextLevel->starts_with('*'))
        {
            // The finale's fade leaves the ramp black; ShowNetworkFailure (00446120) starts by setting the user's gamma (GammaSet at 0045ffc0), and the pages keep it.
            Renderer.ResetGamma();
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
                ShowTextPage(Window, Renderer, Archive, *Panel, "common\\data\\extra\\theend.tga", std::format("common\\data\\extra\\theend-{}.txt", TextSuffix()), FirstPageLeft);
                ShowTextPage(Window, Renderer, Archive, *Panel, "common\\data\\extra\\theend.tga", std::format("common\\data\\extra\\theend-{}-2.txt", TextSuffix()), SecondPageLeft);
            }

            ShowFinalScore();

            return FLevelEnd{ .Kind = ELevelEnd::Menu };
        }

        // The level ends once the frame in which the player took a lift has faded to black.
        if (NextLevel && Player && PlayerObject)
        {
            constexpr int FadeQuarters = 4;
            constexpr uint32_t GammaFadeStepMilliseconds = 50;
            constexpr uint32_t LineFadeStepMilliseconds = 25;
            if (!LiftFadeStep)
            {
                LiftFadeStep = 0;
                MenuFadeTime = ClockMilliseconds();
            }
            else if (ClockMilliseconds() - MenuFadeTime >= (Settings.Details.bGammaFx ? GammaFadeStepMilliseconds : LineFadeStepMilliseconds))
            {
                MenuFadeTime = ClockMilliseconds();
                ++*LiftFadeStep;
            }

            if (*LiftFadeStep >= FadeQuarters)
            {
                Carried = CarriedNow();
                return FLevelEnd{ .Kind = ELevelEnd::Lift, .NextLevel = *NextLevel };
            }
        }
    }

    return FLevelEnd{};
}
}

int main(const int ArgCount, char** const Args)
{
    // Every argument main takes; a new one belongs here too.
    constexpr std::string_view Help = R"(Usage: Vyruz [<game folder>] [options]

Options take one dash or two and any case: -skipIntro, --skipintro and --SkipIntro are the same.

  <game folder>             The folder with vz-data.rsc (or the archive itself), remembered for later runs.
                            Without it: vz-data.rsc beside the executable, else the remembered folder, else a folder picker.

Playing:
  --level <level>           Start straight in a level, skipping the menu: a campaign number from 1 to 24, a level id such as 2.3, or a path such as cave\t2c1.
                            A bad value lists them all.
  --start <x>,<y>           Put the player at that world position as the level starts; skips the menu.
  --skipIntro               Go straight to the main menu, without the story page and the intro.
                            Extras still plays the intro.
  --windowed                Start in a window instead of fullscreen; Alt+Enter or F11 switches either way.

Cheats (combine freely, any case; never applied to the demo):
  --godMode                 Energy and shield stay full, so nothing can kill the player.
  --infiniteAmmo            Every ammo the player holds stays full.
  --allWeapons              Every weapon is armed with full ammo at each level start (--allWeapon works too).
  --infiniteMoney           Cash stays at 999999.

Tools (each comes first, runs on its own and exits; anything else on the line is an error):
  --extract <game folder | archive.rsc> [<output folder>]
                            Write the archives as modern files: sprite sheets, JSON, BMFont, WAV, text and Tiled maps.
                            The default output is Extracted beside the input.
  --compress <extracted folder> [<output folder>]
                            Pack an extracted tree, edits included, back into archives the original reads.
                            The default output is Packed beside the tree.
  --verify-extract <game folder>
                            Extract, pack back and check that everything reads the same.
  --verify-gaf, --verify-levels, --verify-game, --verify-intro
                            Decode every sprite file, load every level, replay the demo against the original's records, or decode the intro.
                            Each exits non-zero on any failure.
  --dump-gaf <archive path> List a sprite file's animations with their frame counts and durations.

  -h, --help                Show this list.
)";
    // Options take one dash or two and any case (-skipIntro is --skipintro); values and paths stay as given.
    std::vector<std::string> Arguments;
    for (int Index = 1; Index < ArgCount; ++Index)
    {
        std::string Argument = Args[Index];
        if (Argument.size() > 2 && Argument[0] == '-' && std::isalpha(uint8_t(Argument[1])))
        {
            Argument.insert(0, "-");
        }

        if (Argument.starts_with("--"))
        {
            std::ranges::transform(Argument, Argument.begin(), [](const char Character) { return char(std::tolower(uint8_t(Character))); });
        }

        Arguments.push_back(std::move(Argument));
    }

    const auto Fail = [](const std::string& Message)
    {
        AttachParentConsole();
        std::fprintf(stderr, "%s\nRun with --help for every argument.\n", Message.c_str());
        return 1;
    };

    if (std::ranges::any_of(Arguments, [](const std::string& Argument) { return Argument == "-h" || Argument == "--help"; }))
    {
        AttachParentConsole();
        std::fputs(Help.data(), stdout);
        return 0;
    }

    // A tool runs alone: it comes first and takes only its own operands, none of them an option.
    constexpr std::array<std::string_view, 3> Tools = { "--extract", "--compress", "--verify-extract" };
    for (size_t Index = 1; Index < Arguments.size(); ++Index)
    {
        if (std::ranges::find(Tools, Arguments[Index]) != Tools.end())
        {
            return Fail(std::format("{} runs on its own and must come first.", Arguments[Index]));
        }
    }

    if (!Arguments.empty() && std::ranges::find(Tools, Arguments.front()) != Tools.end())
    {
        const std::string& Tool = Arguments.front();
        const size_t MaxOperands = Tool == "--verify-extract" ? 1 : 2;
        const std::span<const std::string> Operands = std::span(Arguments).subspan(1);
        if (Operands.empty() || Operands.size() > MaxOperands || std::ranges::any_of(Operands, [](const std::string& Operand) { return Operand.starts_with("--"); }))
        {
            return Fail(Tool == "--extract"    ? "Usage: --extract <game folder | archive.rsc> [<output folder>]"
                        : Tool == "--compress" ? "Usage: --compress <extracted folder> [<output folder>]"
                                               : "Usage: --verify-extract <game folder>");
        }

        const std::optional<std::filesystem::path> Output = Operands.size() > 1 ? std::optional<std::filesystem::path>(Operands[1]) : std::nullopt;
        if (Tool == "--extract")
        {
            // The archives as modern files, then exit.
            return RunExtract(Operands[0], Output);
        }

        if (Tool == "--compress")
        {
            // The extracted tree packed back into archives the original reads, then exit.
            return RunCompress(Operands[0], Output);
        }

        // Extract, pack back and compare, then edit a text, an image and a sprite and check they pack.
        AttachParentConsole();
        return VerifyExtractRoundTrip(Operands[0]);
    }

    std::optional<std::filesystem::path> DataPath;
    std::string LevelPath = "underground\\t1u1";
    // Development aid: --start X,Y spawns the player at that world position.
    std::optional<std::pair<float, float>> Start;
    bool bVerifyGafs = false;
    bool bVerifyLevels = false;
    bool bVerifyGame = false;
    bool bVerifyIntro = false;
    // --level and --start skip the main menu at startup, --skipIntro only the intro before it.
    bool bSkipMenu = false;
    bool bSkipIntro = false;
    bool bWindowed = false;
    // Development aid: --dump-gaf PATH lists an archive GAF's animations with their frame counts and durations.
    std::optional<std::string> DumpGafPath;
    FCheats Cheats;
    const std::array<std::pair<std::string_view, bool*>, 7> Switches = { {
        { "--verify-gaf", &bVerifyGafs },
        { "--verify-levels", &bVerifyLevels },
        { "--verify-game", &bVerifyGame },
        { "--verify-intro", &bVerifyIntro },
        { "--skipintro", &bSkipIntro },
        { "--skip-intro", &bSkipIntro },
        { "--windowed", &bWindowed },
    } };
    for (size_t Index = 0; Index < Arguments.size(); ++Index)
    {
        const std::string& Argument = Arguments[Index];
        // A value never reads as an option, except --start's negative coordinates.
        const auto IsOption = [](const std::string& Text) { return Text.starts_with("--") || Text == "-h"; };
        const bool bHasValue = Index + 1 < Arguments.size() && !IsOption(Arguments[Index + 1]);
        const auto Switch = std::ranges::find(Switches, std::string_view(Argument), &std::pair<std::string_view, bool*>::first);
        if (Switch != Switches.end())
        {
            *Switch->second = true;
        }
        else if (bool FCheats::* const Cheat = CheatFlag(Argument))
        {
            Cheats.*Cheat = true;
        }
        else if (Argument == "--dump-gaf" || Argument == "--level" || Argument == "--start")
        {
            if (!bHasValue)
            {
                return Fail(std::format("{} needs a value.", Argument));
            }

            const std::string& Value = Arguments[++Index];
            float X = 0.f;
            float Y = 0.f;
            if (Argument == "--dump-gaf")
            {
                DumpGafPath = Value;
            }
            else if (Argument == "--start" && std::sscanf(Value.c_str(), "%f,%f", &X, &Y) == 2)
            {
                Start = { X, Y };
                bSkipMenu = true;
            }
            else if (Argument == "--start")
            {
                return Fail("--start takes a world position as X,Y.");
            }
            else if (const std::optional<std::string> Level = LevelArgument(Value))
            {
                LevelPath = *Level;
                bSkipMenu = true;
            }
            else
            {
                std::string Message = std::format("--level takes a number from 1 to {}, a level id such as 2.3, or a path such as cave\\t2c1:", CampaignLevels.size());
                for (size_t Number = 0; Number < CampaignLevels.size(); ++Number)
                {
                    Message += std::format("\n  {:2}  {:<5} {}", Number + 1, CampaignLevels[Number].first, CampaignLevels[Number].second);
                }

                return Fail(Message);
            }
        }
        else if (IsOption(Argument) || Argument.starts_with("-"))
        {
            return Fail(std::format("Unknown option {}.", Args[Index + 1]));
        }
        else if (DataPath)
        {
            return Fail(std::format("Two game folders given: {} and {}.", DataPath->string(), Argument));
        }
        else
        {
            DataPath = Argument;
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
        const bool bBoundKeys = VerifyBoundKeys();
        std::printf("Bound keys page: %s\n", bBoundKeys ? "passed" : "failed");
        const std::expected<std::string, std::string> InfoScreens = VerifyInfoScreens(Archive);
        std::printf("Inventory and help: %s\n", InfoScreens ? InfoScreens->c_str() : InfoScreens.error().c_str());
        std::printf("Frame loop: %s, terrain collision: %s, player movement: %s\n", bFrameLoop ? "passed" : "failed",
            Collision ? "passed" : Collision.error().c_str(), Movement ? "passed" : Movement.error().c_str());
        return bFrameLoop && Collision && Movement && Paths && Demo && Combat && Patrols && Doors && SessionCheck && bScores && bBoundKeys && InfoScreens ? 0 : 1;
    }

    if (!glfwInit())
    {
        std::fprintf(stderr, "glfwInit failed\n");
        return 1;
    }

    // GLFW's built-in mappings miss many pads, 8BitDo's among them, which then read only as plain joysticks.
    for (const char* const Mapping : GamepadMappings)
    {
        glfwUpdateGamepadMappings(Mapping);
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    // Wayland compositors and X11 window managers find the icon through the desktop entry named after this id (Resources/Linux/openvyruz.desktop).
    constexpr const char* AppId = "openvyruz";
    glfwWindowHintString(GLFW_WAYLAND_APP_ID, AppId);
    glfwWindowHintString(GLFW_X11_CLASS_NAME, AppId);
#ifdef _WIN32
    // GLFW keeps a Win32 fullscreen window topmost, so it must minimize when another window takes the focus, or the paused game covers it.
#else
    // Fullscreen stays up when another window takes the focus; the game pauses instead, as it does in a window.
    glfwWindowHint(GLFW_AUTO_ICONIFY, GLFW_FALSE);
#endif
#ifdef _WIN32
    // Hidden until it has gone fullscreen and shown a black frame, so the desktop never sees its white windowed client area.
    // Wayland maps a surface only once it is shown, so there the window starts visible.
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
#endif
    GLFWwindow* const Window = glfwCreateWindow(WindowedPlace.Width, WindowedPlace.Height, "Vyruz", nullptr, nullptr);
    if (!Window)
    {
        std::fprintf(stderr, "glfwCreateWindow failed\n");
        return 1;
    }

    if (!bWindowed)
    {
        SetFullscreen(Window, true);
    }

    WatchMouseWheel(Window);

    FRenderer Renderer;
    Renderer.Init(Window);
#ifdef _WIN32
    if (Renderer.BeginFrame())
    {
        Renderer.EndFrame();
    }

    // Shown late, it does not come to the front by itself, and the game stands still without the focus.
    glfwShowWindow(Window);
    glfwFocusWindow(Window);
#endif
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

    FSettings Settings = LoadSettings(Archive, DataPath->parent_path());
    // A vzconf.txt asking for Russian over the English data plays in English; the setting itself stays, for the original and the Russian data.
    SetLanguage(ELanguage(Settings.Language) == ELanguage::Russian && (!MenuArt || !MenuArt->HasCyrillic()) ? ELanguage::English : ELanguage(Settings.Language));
    Renderer.SetUserGamma(Settings.Gamma);
    if (Panel)
    {
        Panel->SetDetails(Settings.Details);
    }

    // VzRun at 00446e20 opens the sound (VzOpenSound (004477e0)) and applies the volumes; the music waits for the story page and the intro.
    FAudio Audio(Archive);
    if (Audio.Open(Settings.SoundFormat))
    {
        GameAudio = &Audio;
        Audio.Apply(Settings.Audio);
    }

    GameFolder = DataPath->parent_path();
    Settings.Folder = PlayerSessionFolder(GameFolder, Settings.PlayerName);
    std::optional<FCarriedPlayer> Carried;
    std::optional<FSession> Session = ReadSession(DefaultSessionFolder(Settings.Folder));
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
    // VzRun shows the story page (ShowStoryPage at 00447c80, read aloud, for up to 30 seconds), then plays the intro, before the main menu; --skipIntro skips both.
    if (bMenu && Panel && !bSkipIntro)
    {
        constexpr int32_t StoryLeft = 0x80;
        constexpr uint32_t StoryMilliseconds = 30000;
        if (GameAudio)
        {
            GameAudio->PlayStoryVoice();
        }

        ShowTextPage(Window, Renderer, Archive, *Panel, "common\\data\\extra\\story.tga", std::format("common\\data\\extra\\story-{}.txt", TextSuffix()), StoryLeft, StoryMilliseconds);
        if (GameAudio)
        {
            GameAudio->StopStoryVoice();
        }

        Renderer.ReleaseSpriteSheets();
    }

    if (bMenu && !bSkipIntro && FindIntro(DataPath->parent_path()))
    {
        PlayIntro(Window, Renderer, DataPath->parent_path());
    }

    // Then MusicLoad and MusicStart, before VzMenuLoop.
    if (GameAudio)
    {
        GameAudio->StartMusic();
    }

    // Whether the next level starts a new game, which shows its level page.
    bool bNewGame = false;
    for (std::string Path = LevelPath;;)
    {
        if (bMenu)
        {
            FSession Chosen;
            const EMenuChoice Choice = RunMainMenu(Window, Renderer, Archive, *MenuArt, Session, Settings, DataPath->parent_path(), Chosen);
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
                bNewGame = Choice == EMenuChoice::NewGame;
                Carried.reset();
                Restore = Choice == EMenuChoice::NewGame ? std::nullopt : std::optional(std::move(Chosen));
                bMenu = false;
            }
        }

        // The level's fade in (FUN_0041e5a0) centres the pans.
        if (GameAudio)
        {
            GameAudio->SetLevelZone(Path);
            GameAudio->CenterPans();
        }

        const std::expected<FLevelEnd, std::string> End = RunLevel(Window, Renderer, Archive, Path, Start, Cheats, Carried, DebugLevels, Panel ? &*Panel : nullptr,
            MenuArt ? &*MenuArt : nullptr, Session, Restore ? &*Restore : nullptr, Settings, Demo ? &*Demo : nullptr, bNewGame || Carried.has_value());
        bNewGame = false;
        Restore.reset();
        Renderer.ReleaseSpriteSheets();
        // EngineRunLevel stops the player's weapon and motor, and LoopLevels brings the first tune back.
        if (GameAudio)
        {
            GameAudio->SetLevelZone({});
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
            Carried.reset();
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
