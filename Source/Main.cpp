#include "Game/Level.h"
#include "Pg/Gaf.h"
#include "Pg/ResourceArchive.h"
#include "Render/Renderer.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
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
constexpr int64_t InGroundDepthLayer = 3;
constexpr int64_t ZShadowDepthOffset = 64;
constexpr uint32_t AnimationBlendMask = 0xFE;
constexpr uint32_t AdditiveDrawMode = 6;
constexpr uint32_t InverseSourceDrawMode = 8;
constexpr int FourWayDirectionCount = 4;

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
    // HeliSelectBodyPose at 004334e0 uses movement speed rather than facing; its initial pose is zero.
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
        // Index 0 is the unused placeholder; any other untyped descriptor never reaches a .3DI slot.
        const ptrdiff_t Untyped = std::ranges::count(Level->TileTypes, ETerrain::None) - 1;
        std::printf("%-16s %ux%u, %u layers, %zu blocks, %zu tile types (%td untyped), %zu objects, %u missing tile refs, \"%s\"\n", LevelPath.c_str(),
            Level->GridWidth, Level->GridHeight, Level->LayerCount, Level->Blocks.size(), Level->TileTypes.size() - 1, Untyped, Level->Objects.size(),
            Level->MissingTileRefs, Level->Name.c_str());
        ++Loaded;
    }
    std::printf("Levels: %d loaded, %d failed\n", Loaded, Failed);
    return Failed == 0 && Loaded > 0 ? 0 : 1;
}

struct FObjectSprite
{
    size_t SheetIndex = 0;
    Gaf::FAnimationFrame Frame;
    float X = 0.f;
    float Y = 0.f;
    int64_t Depth = 0;
    ESpriteBlend Blend = ESpriteBlend::Opaque;
};

struct FObjectSprites
{
    std::vector<FSpriteSheet> Sheets;
    std::vector<FObjectSprite> Sprites;
};

// shortcut: initial poses only, use the behaviour update for animation state changes and attachments.
std::expected<FObjectSprites, std::string> CreateObjectSprites(FResourceArchive& Archive, FRenderer& Renderer, const FLevel& Level)
{
    FObjectSprites Result;
    std::unordered_map<std::string, size_t> SheetIndices;
    std::vector<std::vector<Gaf::FAnimation>> Animations;
    for (const FLevelObject& Object : Level.Objects)
    {
        for (const bool bShadow : { true, false })
        {
            const std::string& Path = bShadow ? Object.ShadowGafPath : Object.GafPath;
            const int BaseIndex = bShadow ? Object.ShadowIndex : Object.GafIndex;
            if (Path.empty() || BaseIndex == 0)
            {
                continue;
            }

            auto [Sheet, bInserted] = SheetIndices.try_emplace(Path, Result.Sheets.size());
            if (bInserted)
            {
                std::optional<FObjectGaf> Gaf = LoadObjectGaf(Archive, Path);
                if (!Gaf)
                {
                    return std::unexpected("Cannot decode object GAF " + Path);
                }

                Result.Sheets.push_back(Renderer.CreateSpriteSheet(Gaf->Bitmaps));
                Animations.push_back(std::move(Gaf->Animations));
            }

            const auto& GafAnimations = Animations[Sheet->second];
            const size_t Index = ObjectAnimationIndex(Object, bShadow);
            if (Index >= GafAnimations.size())
            {
                if (GafAnimations.empty() && Result.Sheets[Sheet->second].Frames.empty())
                {
                    continue;
                }

                return std::unexpected("Invalid animation index for " + Object.ClassName + " in " + Path);
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

            const int64_t GroundY = int64_t(Object.Y);
            Result.Sprites.push_back({
                .SheetIndex = Sheet->second,
                .Frame = Animation.Frames.front(),
                .X = Object.X,
                .Y = Object.Y - (bShadow ? 0.f : Object.Altitude),
                .Depth = SpriteDepth(GroundY, bShadow, Object.bInGround, Object.bZShadow),
                .Blend = DrawMode == AdditiveDrawMode ? ESpriteBlend::Additive : DrawMode == InverseSourceDrawMode ? ESpriteBlend::InverseSourceColor : ESpriteBlend::Opaque,
            });
        }
    }

    std::stable_sort(Result.Sprites.begin(), Result.Sprites.end(), [](const FObjectSprite& A, const FObjectSprite& B) { return SpriteDrawsBefore(A.Depth, B.Depth); });
    return Result;
}
}

int main(const int ArgCount, char** const Args)
{
    // Defaults to the original-data repository checked out next to this one; pass another directory as an argument.
    std::filesystem::path DataRoot = "../Vyruz/Game/Vyruz";
    std::string LevelPath = "underground\\t1u1";
    bool bVerifyGafs = false;
    bool bVerifyLevels = false;
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
        else if (std::strcmp(Args[Index], "--level") == 0 && Index + 1 < ArgCount)
        {
            LevelPath = Args[++Index];
        }
        else
        {
            DataRoot = Args[Index];
        }
    }

    FResourceArchive Archive;
    if (!Archive.Open(DataRoot / "vz-data.rsc"))
    {
        std::fprintf(stderr, "Cannot open %s\n", (DataRoot / "vz-data.rsc").string().c_str());
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
    const std::expected<FLevel, std::string> Level = LoadLevel(Archive, LevelPath);
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

    // Static level view until the game loop is ported: the MAP_ layers drawn bottom-up, scrolled with the arrow keys.
    const FSpriteSheet Blocks = Renderer.CreateSpriteSheet(Level->Blocks);
    const std::expected<FObjectSprites, std::string> Objects = CreateObjectSprites(Archive, Renderer, *Level);
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
    const auto Player = std::ranges::find_if(Level->Objects, [](const FLevelObject& Object) { return Object.ClassName == "Vyruz"; });
    double CameraX = Player != Level->Objects.end() ? Player->X - ScreenWidth * 0.5 : 0.0;
    double CameraY = Player != Level->Objects.end() ? Player->Y - ScreenHeight * 0.5 : 0.0;
    std::printf("%s: %s, %zu objects, %zu sprite parts\n", LevelPath.c_str(), Level->Name.c_str(), Level->Objects.size(), Objects->Sprites.size());
    std::fflush(stdout);

    double LastTime = glfwGetTime();
    while (!glfwWindowShouldClose(Window))
    {
        glfwPollEvents();
        const double Now = glfwGetTime();
        const double Step = (Now - LastTime) * ScrollSpeed;
        LastTime = Now;
        CameraX += (glfwGetKey(Window, GLFW_KEY_RIGHT) - glfwGetKey(Window, GLFW_KEY_LEFT)) * Step;
        CameraY += (glfwGetKey(Window, GLFW_KEY_DOWN) - glfwGetKey(Window, GLFW_KEY_UP)) * Step;
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
        for (uint32_t Layer = 0; Layer < Level->LayerCount; ++Layer)
        {
            const uint32_t* const Cells = Level->Map.data() + size_t{ Layer } * Level->GridWidth * Level->GridHeight;
            for (uint32_t Y = FirstY; Y < EndY; ++Y)
            {
                for (uint32_t X = FirstX; X < EndX; ++X)
                {
                    // MAP_ block indices are 1-based; 0 leaves the cell empty.
                    if (const uint32_t Block = Cells[size_t{ Y } * Level->GridWidth + X])
                    {
                        Renderer.DrawSprite(Blocks, Block - 1, float(int(X * BlockWidth) - OriginX), float(int(Y * BlockHeight) - OriginY));
                    }
                }
            }
        }

        for (const FObjectSprite& Sprite : Objects->Sprites)
        {
            const Gaf::FAnimationFrame& Frame = Sprite.Frame;
            Renderer.DrawSprite(Objects->Sheets[Sprite.SheetIndex], Frame.BitmapIndex - 1, Sprite.X + Frame.OffsetX - OriginX,
                Sprite.Y + Frame.OffsetY - OriginY, Frame.bMirrorX, Frame.bMirrorY, Sprite.Blend);
        }

        Renderer.EndFrame();
    }

    Renderer.Shutdown();
    glfwDestroyWindow(Window);
    glfwTerminate();
    return 0;
}
