#include "Pg/Gaf.h"
#include "Pg/ResourceArchive.h"
#include "Render/Renderer.h"

#include <GLFW/glfw3.h>

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

FSpriteSheet LoadSpriteSheet(FResourceArchive& Archive, FRenderer& Renderer, const char* const Name)
{
    const std::optional<std::vector<uint8_t>> File = Archive.Read(Name);
    const std::optional<std::vector<Gaf::FFrame>> Frames = File ? Gaf::LoadFrames(*File) : std::nullopt;
    if (!Frames)
    {
        std::fprintf(stderr, "Cannot load %s\n", Name);
        return {};
    }
    return Renderer.CreateSpriteSheet(*Frames);
}
}

int main(const int ArgCount, char** const Args)
{
    // Defaults to the original-data repository checked out next to this one; pass another directory as an argument.
    std::filesystem::path DataRoot = "../Vyruz/Game/Vyruz";
    bool bVerifyGafs = false;
    for (int Index = 1; Index < ArgCount; ++Index)
    {
        if (std::strcmp(Args[Index], "--verify-gaf") == 0)
        {
            bVerifyGafs = true;
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

    // Sprite viewer until the game loop is ported: every player ship frame in a grid, plus a looping explosion.
    const FSpriteSheet Ship = LoadSpriteSheet(Archive, Renderer, "COMMON\\DATA\\OBJECT\\VYRUZ\\VZ.GAF");
    const FSpriteSheet Explosion = LoadSpriteSheet(Archive, Renderer, "COMMON\\DATA\\OBJECT\\FX\\EXPLO.GAF");
    constexpr double AnimationFps = 15.0;
    constexpr float CellSize = 64.0f;

    while (!glfwWindowShouldClose(Window))
    {
        glfwPollEvents();
        if (!Renderer.BeginFrame())
        {
            glfwWaitEventsTimeout(0.1);
            continue;
        }

        const int Columns = static_cast<int>(ScreenWidth / CellSize);
        for (size_t Index = 0; Index < Ship.Frames.size(); ++Index)
        {
            Renderer.DrawSprite(Ship, Index, static_cast<float>(Index % Columns) * CellSize, static_cast<float>(Index / Columns) * CellSize);
        }
        if (!Explosion.Frames.empty())
        {
            const size_t Frame = static_cast<size_t>(glfwGetTime() * AnimationFps) % Explosion.Frames.size();
            Renderer.DrawSprite(Explosion, Frame, ScreenWidth * 0.5f - 64.0f, ScreenHeight - 160.0f);
        }

        Renderer.EndFrame();
    }

    Renderer.Shutdown();
    glfwDestroyWindow(Window);
    glfwTerminate();
    return 0;
}
