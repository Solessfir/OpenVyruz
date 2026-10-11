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
#include <vector>

namespace
{
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
        const std::optional<std::vector<uint8_t>> File = Archive.Read(Name);
        const std::optional<std::vector<Gaf::FFrame>> Frames = File ? Gaf::LoadFrames(*File) : std::nullopt;
        if (!Frames)
        {
            std::printf("FAIL %s\n", Name.c_str());
            ++Failed;
            continue;
        }
        ++Decoded;
        FrameCount += Frames->size();
    }
    std::printf("GAF: %d decoded (%zu frames), %d failed\n", Decoded, FrameCount, Failed);
    return Failed == 0 ? 0 : 1;
}

// Loads every level in the archive plus the animated map GAFs its .3DX references.
int VerifyLevels(FResourceArchive& Archive)
{
    int Loaded = 0;
    int Failed = 0;
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
    constexpr float BlockWidth = 64.f;
    constexpr float BlockHeight = 32.f;
    constexpr double ScrollSpeed = 600.0;
    const float MapWidth = Level->GridWidth * BlockWidth;
    const float MapHeight = Level->GridHeight * BlockHeight;
    const auto Player = std::ranges::find_if(Level->Objects, [](const FLevelObject& Object) { return Object.ClassName == "Vyruz"; });
    double CameraX = Player != Level->Objects.end() ? Player->X - ScreenWidth * 0.5 : 0.0;
    double CameraY = Player != Level->Objects.end() ? Player->Y - ScreenHeight * 0.5 : 0.0;
    std::printf("%s: %s\n", LevelPath.c_str(), Level->Name.c_str());
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
        const uint32_t FirstX = uint32_t(OriginX / int(BlockWidth));
        const uint32_t FirstY = uint32_t(OriginY / int(BlockHeight));
        const uint32_t LastX = std::min(Level->GridWidth, FirstX + uint32_t(ScreenWidth / BlockWidth) + 2);
        const uint32_t LastY = std::min(Level->GridHeight, FirstY + uint32_t(ScreenHeight / BlockHeight) + 2);
        for (uint32_t Layer = 0; Layer < Level->LayerCount; ++Layer)
        {
            const uint32_t* const Cells = Level->Map.data() + size_t{ Layer } * Level->GridWidth * Level->GridHeight;
            for (uint32_t Y = FirstY; Y < LastY; ++Y)
            {
                for (uint32_t X = FirstX; X < LastX; ++X)
                {
                    if (const uint32_t Block = Cells[size_t{ Y } * Level->GridWidth + X])
                    {
                        Renderer.DrawSprite(Blocks, Block - 1, X * BlockWidth - OriginX, Y * BlockHeight - OriginY);
                    }
                }
            }
        }

        Renderer.EndFrame();
    }

    Renderer.Shutdown();
    glfwDestroyWindow(Window);
    glfwTerminate();
    return 0;
}
