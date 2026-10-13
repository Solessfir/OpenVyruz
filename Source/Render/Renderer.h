#pragma once

#include "Pg/Gaf.h"

#include <GLFW/glfw3.h>

#include <cstdint>
#include <vector>

// The original game renders at 640x480 (VZCONF.TXT video-driver-device-mode); the window letterboxes that space.
inline constexpr float ScreenWidth = 640.0f;
inline constexpr float ScreenHeight = 480.0f;

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
    Count,
};

class FRenderer
{
public:
    bool Init(GLFWwindow* Window);
    void Shutdown();

    FSpriteSheet CreateSpriteSheet(const std::vector<Gaf::FFrame>& Frames);
    // Frees every sprite sheet, as a level change does; earlier sheets must not be drawn again.
    void ReleaseSpriteSheets();

    // Returns false when nothing can be drawn this frame (minimized or swapchain being rebuilt).
    bool BeginFrame();
    // X/Y is the top-left of the untrimmed sprite cell in screen pixels.
    // VisibleColumns keeps only that many columns of the bitmap from its left edge, as a clipped panel bar.
    // Diffuse multiplies the bitmap and Specular is added to it, both as 0xRRGGBB.
    void DrawSprite(const FSpriteSheet& Sheet, size_t FrameIndex, float X, float Y, bool bMirrorX = false, bool bMirrorY = false,
        ESpriteBlend Blend = ESpriteBlend::Opaque, float ScaleX = 1.f, float ScaleY = 1.f, uint32_t VisibleColumns = UINT32_MAX,
        uint32_t Diffuse = 0xffffff, uint32_t Specular = 0, float Alpha = 1.f);
    void EndFrame();

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
    };

    void CreateInstance();
    void CreateDevice();
    void CreatePipeline();
    bool CreateSwapchain();
    void DestroySwapchainResources();
    uint32_t FindMemoryType(uint32_t TypeBits, VkMemoryPropertyFlags Properties) const;
    uint32_t CreateTexture(uint32_t Width, uint32_t Height, const std::vector<uint32_t>& Rgba);

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
};
