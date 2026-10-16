#pragma once

#include "Pg/Gaf.h"

#include <GLFW/glfw3.h>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The original game renders at 640x480 (VZCONF.TXT video-driver-device-mode); the window letterboxes that space.
inline constexpr float ScreenWidth = 640.0f;
inline constexpr float ScreenHeight = 480.0f;

// The port's wide view: the original's 480 rows across as many columns as the window's shape gives, never fewer than 640, so a wide window shows more of the level instead of stretching it.
float WideViewWidth(GLFWwindow* Window);

struct FSpriteFrame
{
    float U0 = 0.0f;
    float V0 = 0.0f;
    float U1 = 0.0f;
    float V1 = 0.0f;
    uint32_t Width = 0;
    uint32_t Height = 0;
    int32_t TrimX = 0;
    int32_t TrimY = 0;
};

// All frames of one GAF packed into a single texture.
struct FSpriteSheet
{
    uint32_t Texture = UINT32_MAX;
    std::vector<FSpriteFrame> Frames;
};

enum class ESpriteBlend : uint8_t
{
    Opaque,
    Additive,
    InverseSourceColor,
    // Draw modes 2 and 3 blend by an alpha the caller gives.
    Alpha,
    // Lifts the destination toward white by the source: destination + source * (1 - destination), for the gamma offsets.
    Screen,
    // Adds the destination to itself by the source: destination + destination * source, as ScoreHighlightRow blends a row onto itself.
    Double,
    Count,
};

// What draws do to the stencil, which marks the parts of the player and the enemies that the terrain covers.
enum class EStencilMode : uint8_t
{
    Off,
    // Terrain marks what it covers.
    MarkTerrain,
    // The player and the enemies unmark their own pixels, so only terrain drawn after them marks them again.
    ClearEntity,
    // Draws a sprite's subtle outline, its edge and a faint fill in the diffuse colour, only where the terrain covers it.
    Outline,
};

class FRenderer
{
public:
    bool Init(GLFWwindow* Window);
    void Shutdown();

    FSpriteSheet CreateSpriteSheet(const std::vector<Gaf::FFrame>& Frames);
    // Frees every sprite sheet, as a level change does; earlier sheets must not be drawn again.
    void ReleaseSpriteSheets();
    // A 256x256 grey frame, Brightness (0 to 1) of the distance from its middle, 1 at the edge's middle; for the port's lights and screen edges.
    FSpriteSheet CreateGradientSheet(const std::function<float(float)>& Brightness);
    // A sheet of one Width by Height frame whose pixels UpdateVideoSheet replaces, for video.
    FSpriteSheet CreateVideoSheet(uint32_t Width, uint32_t Height);
    // Rgba is R, G, B and A bytes for each of the sheet's pixels.
    void UpdateVideoSheet(const FSpriteSheet& Sheet, std::span<const uint32_t> Rgba);

    // Returns false when nothing can be drawn this frame (minimized or swapchain being rebuilt).
    // ViewWidth is the frame's width in screen pixels over the original 480 rows; the screens other than the level keep the 640 of the original.
    bool BeginFrame(float ViewWidth = ScreenWidth);
    // X/Y is the top-left of the untrimmed sprite cell in screen pixels.
    // VisibleColumns keeps only that many columns of the bitmap from its left edge, as a clipped panel bar.
    // Diffuse multiplies the bitmap and Specular is added to it, both as 0xRRGGBB.
    // Moves every sprite drawn after it by a fraction of a pixel, so the level can follow the player smoothly while its objects keep their whole-pixel places; each frame starts with none.
    void SetViewShift(float X, float Y);
    void DrawSprite(const FSpriteSheet& Sheet, size_t FrameIndex, float X, float Y, bool bMirrorX = false, bool bMirrorY = false,
        ESpriteBlend Blend = ESpriteBlend::Opaque, float ScaleX = 1.f, float ScaleY = 1.f, uint32_t VisibleColumns = UINT32_MAX,
        uint32_t Diffuse = 0xffffff, uint32_t Specular = 0, float Alpha = 1.f, float RotationDegrees = 0.f, float PivotX = 0.f, float PivotY = 0.f, bool bSoftShadow = false);
    // Blurs what this frame has drawn so far and multiplies it by Tint (0xRRGGBB), for an overlay over the game; without the device support it only darkens.
    void BlurView(uint32_t Tint);
    // Adds a soft glow around what additive sprites drew this frame and nothing has covered since, as the view's alpha marks it.
    void Bloom();
    // The port's hit flash: red and blue part from green toward the view's edges, by Strength from 0 to 1.
    void Aberrate(float Strength);
    // ScoreHighlightRow at 00401410: blends what is drawn in the rectangle onto itself, twice as bright, saturating.
    void DoubleRect(float X, float Y, float Width, float Height);
    // Draws the gamma ramp over the frame and presents it.
    void EndFrame();
    // Each frame starts Off. Without a stencil format on the device the mode stays Off and CanOutline is false.
    void SetStencilMode(EStencilMode Mode);
    bool CanOutline() const { return StencilFormat != VK_FORMAT_UNDEFINED; }

    // The gamma ramp (FUN_0045ffc0): red, green and blue from -255 to 255 over the whole picture, a positive value lifting its channel toward white by value / 256 of the way and a negative one scaling it by (value + 255) / 256.
    void SetGamma(const std::array<int32_t, 3>& Ramp);
    // The user's gamma (session +0x27d4), which the menus give the ramp back (FUN_00420990).
    void SetUserGamma(const std::array<int32_t, 3>& Ramp);
    void ResetGamma();
    const std::array<int32_t, 3>& GetGamma() const;
    const std::array<int32_t, 3>& GetUserGamma() const;
    // The Vulkan device's name, which the Video menu shows as its device.
    std::string GetDeviceName() const;
    // Copies the middle 640x480 of the next finished frame's view for TakeCapture, which shrinks it to Width by Height.
    // The copy is taken before the gamma, which the original's hardware ramp kept out of its surfaces.
    void RequestCapture(uint32_t Width, uint32_t Height);
    // The requested picture as RGB555 from the top row down once a frame has been drawn; nullopt before then or when the window cannot be read back.
    std::optional<std::vector<uint16_t>> TakeCapture();

private:
    static constexpr uint32_t FramesInFlight = 2;

    struct FFrame
    {
        VkCommandBuffer CommandBuffer = VK_NULL_HANDLE;
        VkSemaphore ImageAcquired = VK_NULL_HANDLE;
        VkFence InFlight = VK_NULL_HANDLE;
    };

    struct FTexture
    {
        VkImage Image = VK_NULL_HANDLE;
        VkDeviceMemory Memory = VK_NULL_HANDLE;
        VkImageView View = VK_NULL_HANDLE;
        VkDescriptorSet DescriptorSet = VK_NULL_HANDLE;
        uint32_t Width = 0;
        uint32_t Height = 0;
    };

    void CreateInstance();
    void CreateDevice();
    void CreatePipeline();
    bool CreateSwapchain();
    void DestroySwapchainResources();
    uint32_t FindMemoryType(uint32_t TypeBits, VkMemoryPropertyFlags Properties) const;
    uint32_t CreateTexture(uint32_t Width, uint32_t Height, const std::vector<uint32_t>& Rgba);
    void UploadTexture(VkImage Image, uint32_t Width, uint32_t Height, std::span<const uint32_t> Rgba, VkImageLayout From);
    void DrawGamma();
    const FSpriteSheet& WhitePixel();
    // The view as BeginRendering placed it in the window, in swapchain pixels.
    struct FPlacedView
    {
        int32_t Left = 0;
        int32_t Top = 0;
        VkExtent2D Size{};
    };
    FPlacedView PlaceView() const;
    // The blur's chain from half the view down and the view's full-size copy, made again when the view's size changes.
    void CreateBlurTargets(VkExtent2D View);
    void EnsureViewTargets(VkExtent2D View);
    void DestroyBlurTargets();
    void DrawBlurPass(const FTexture& From, bool bUp, uint32_t Tint, VkPipeline Pipeline, bool bExtract = false);
    // Copies the view down the blur's chain and back up to its first level, then resumes the view for the caller's last pass; a glow takes only the marked part.
    void RunBlurChain(bool bGlow);
    void CreateStencil();
    void DestroyStencil();
    void ApplyStencilMode();
    // Starts drawing into the acquired image, letterboxed; a capture resumes it to add the gamma after the copy.
    void BeginRendering(VkAttachmentLoadOp LoadOp);

    GLFWwindow* Window = nullptr;
    VkInstance Instance = VK_NULL_HANDLE;
    VkSurfaceKHR Surface = VK_NULL_HANDLE;
    VkPhysicalDevice PhysicalDevice = VK_NULL_HANDLE;
    VkDevice Device = VK_NULL_HANDLE;
    uint32_t QueueFamily = 0;
    VkQueue Queue = VK_NULL_HANDLE;
    VkCommandPool CommandPool = VK_NULL_HANDLE;

    VkSwapchainKHR Swapchain = VK_NULL_HANDLE;
    VkFormat SwapchainFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D SwapchainExtent{};
    std::vector<VkImage> SwapchainImages;
    std::vector<VkImageView> SwapchainViews;
    // One per swapchain image, so a semaphore is never reused while the presentation engine still holds it.
    std::vector<VkSemaphore> RenderFinished;
    bool bSwapchainValid = false;

    VkSampler Sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout DescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool DescriptorPool = VK_NULL_HANDLE;
    VkPipelineLayout PipelineLayout = VK_NULL_HANDLE;
    VkPipeline SpritePipelines[static_cast<size_t>(ESpriteBlend::Count)]{};
    VkFormat PipelineFormat = VK_FORMAT_UNDEFINED;
    std::vector<FTexture> Textures;

    FFrame Frames[FramesInFlight];
    uint32_t FrameIndex = 0;
    uint32_t ImageIndex = 0;
    float ViewWidth = ScreenWidth;
    float ShiftX = 0.f;
    float ShiftY = 0.f;

    std::array<int32_t, 3> Gamma{};
    std::array<int32_t, 3> UserGamma{};
    // A white pixel for the gamma pass, made again after ReleaseSpriteSheets.
    std::optional<FSpriteSheet> GammaSheet;

    // The blur's chain, from half the view down, with its own linear sampler and descriptor pool, which outlive the sprite sheets.
    bool bCanBlur = false;
    VkSampler LinearSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout BlurSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool BlurPool = VK_NULL_HANDLE;
    VkPipelineLayout BlurLayout = VK_NULL_HANDLE;
    VkPipeline BlurPipeline = VK_NULL_HANDLE;
    // The blur's last pass draws into the view, whose rendering carries the stencil.
    VkPipeline BlurViewPipeline = VK_NULL_HANDLE;
    VkPipeline BloomPipeline = VK_NULL_HANDLE;
    VkPipeline AberrationPipeline = VK_NULL_HANDLE;

    // The stencil beside the swapchain image, the size of the window.
    VkFormat StencilFormat = VK_FORMAT_UNDEFINED;
    VkImage StencilImage = VK_NULL_HANDLE;
    VkDeviceMemory StencilMemory = VK_NULL_HANDLE;
    VkImageView StencilView = VK_NULL_HANDLE;
    EStencilMode StencilMode = EStencilMode::Off;
    std::vector<FTexture> BlurLevels;
    FTexture ViewCopy;

    // A capture: requested, then copied into a host buffer by EndFrame.
    bool bCanCapture = false;
    std::optional<VkExtent2D> CaptureSize;
    VkBuffer CaptureBuffer = VK_NULL_HANDLE;
    VkDeviceMemory CaptureMemory = VK_NULL_HANDLE;
    VkExtent2D CaptureRegion{};
};
