#include "Pg/Gaf.h"
#include "Pg/ResourceArchive.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{
// The original game runs at 640x480x16 (VZCONF.TXT video-driver-device-mode).
constexpr int OriginalWidth = 640;
constexpr int OriginalHeight = 480;
constexpr uint32_t FramesInFlight = 2;

void Check(const VkResult Result, const char* const What)
{
    if (Result != VK_SUCCESS)
    {
        std::fprintf(stderr, "%s failed: %d\n", What, static_cast<int>(Result));
        std::exit(1);
    }
}

struct FFrame
{
    VkCommandBuffer CommandBuffer = VK_NULL_HANDLE;
    VkSemaphore ImageAcquired = VK_NULL_HANDLE;
    VkFence InFlight = VK_NULL_HANDLE;
};

struct FVulkan
{
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

    FFrame Frames[FramesInFlight];
    uint32_t FrameIndex = 0;
};

void CreateInstance(FVulkan& Vk)
{
    uint32_t GlfwExtensionCount = 0;
    const char** const GlfwExtensions = glfwGetRequiredInstanceExtensions(&GlfwExtensionCount);

    std::vector<const char*> Layers;
#if VYRUZ_DEBUG
    uint32_t LayerCount = 0;
    vkEnumerateInstanceLayerProperties(&LayerCount, nullptr);
    std::vector<VkLayerProperties> Available(LayerCount);
    vkEnumerateInstanceLayerProperties(&LayerCount, Available.data());
    for (const VkLayerProperties& Layer : Available)
    {
        if (std::strcmp(Layer.layerName, "VK_LAYER_KHRONOS_validation") == 0)
        {
            Layers.push_back("VK_LAYER_KHRONOS_validation");
        }
    }
#endif

    const VkApplicationInfo AppInfo{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Vyruz",
        .apiVersion = VK_API_VERSION_1_3,
    };
    const VkInstanceCreateInfo CreateInfo{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &AppInfo,
        .enabledLayerCount = static_cast<uint32_t>(Layers.size()),
        .ppEnabledLayerNames = Layers.data(),
        .enabledExtensionCount = GlfwExtensionCount,
        .ppEnabledExtensionNames = GlfwExtensions,
    };
    Check(vkCreateInstance(&CreateInfo, nullptr, &Vk.Instance), "vkCreateInstance");
}

void CreateDevice(FVulkan& Vk)
{
    uint32_t DeviceCount = 0;
    vkEnumeratePhysicalDevices(Vk.Instance, &DeviceCount, nullptr);
    std::vector<VkPhysicalDevice> Devices(DeviceCount);
    vkEnumeratePhysicalDevices(Vk.Instance, &DeviceCount, Devices.data());

    // Prefer a discrete GPU, otherwise take the first device that can present with Vulkan 1.3.
    int BestScore = -1;
    for (const VkPhysicalDevice Candidate : Devices)
    {
        VkPhysicalDeviceProperties Properties;
        vkGetPhysicalDeviceProperties(Candidate, &Properties);
        if (Properties.apiVersion < VK_API_VERSION_1_3)
        {
            continue;
        }

        uint32_t FamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(Candidate, &FamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> Families(FamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(Candidate, &FamilyCount, Families.data());
        for (uint32_t Family = 0; Family < FamilyCount; ++Family)
        {
            VkBool32 CanPresent = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(Candidate, Family, Vk.Surface, &CanPresent);
            if (!(Families[Family].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !CanPresent)
            {
                continue;
            }

            const int Score = Properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 1 : 0;
            if (Score > BestScore)
            {
                BestScore = Score;
                Vk.PhysicalDevice = Candidate;
                Vk.QueueFamily = Family;
            }
            break;
        }
    }
    if (Vk.PhysicalDevice == VK_NULL_HANDLE)
    {
        std::fprintf(stderr, "No Vulkan 1.3 device with graphics and present support\n");
        std::exit(1);
    }

    const float Priority = 1.0f;
    const VkDeviceQueueCreateInfo QueueInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = Vk.QueueFamily,
        .queueCount = 1,
        .pQueuePriorities = &Priority,
    };
    VkPhysicalDeviceVulkan13Features Features13{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .synchronization2 = VK_TRUE,
        .dynamicRendering = VK_TRUE,
    };
    const char* const Extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    const VkDeviceCreateInfo CreateInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &Features13,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &QueueInfo,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = Extensions,
    };
    Check(vkCreateDevice(Vk.PhysicalDevice, &CreateInfo, nullptr, &Vk.Device), "vkCreateDevice");
    vkGetDeviceQueue(Vk.Device, Vk.QueueFamily, 0, &Vk.Queue);

    const VkCommandPoolCreateInfo PoolInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = Vk.QueueFamily,
    };
    Check(vkCreateCommandPool(Vk.Device, &PoolInfo, nullptr, &Vk.CommandPool), "vkCreateCommandPool");

    for (FFrame& Frame : Vk.Frames)
    {
        const VkCommandBufferAllocateInfo AllocInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = Vk.CommandPool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        Check(vkAllocateCommandBuffers(Vk.Device, &AllocInfo, &Frame.CommandBuffer), "vkAllocateCommandBuffers");

        const VkSemaphoreCreateInfo SemaphoreInfo{ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        Check(vkCreateSemaphore(Vk.Device, &SemaphoreInfo, nullptr, &Frame.ImageAcquired), "vkCreateSemaphore");

        const VkFenceCreateInfo FenceInfo{ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
        Check(vkCreateFence(Vk.Device, &FenceInfo, nullptr, &Frame.InFlight), "vkCreateFence");
    }
}

void DestroySwapchainResources(FVulkan& Vk)
{
    for (const VkImageView View : Vk.SwapchainViews)
    {
        vkDestroyImageView(Vk.Device, View, nullptr);
    }
    for (const VkSemaphore Semaphore : Vk.RenderFinished)
    {
        vkDestroySemaphore(Vk.Device, Semaphore, nullptr);
    }
    Vk.SwapchainViews.clear();
    Vk.RenderFinished.clear();
}

// Returns false while the window is minimized and there is nothing to present to.
bool CreateSwapchain(FVulkan& Vk, GLFWwindow* const Window)
{
    int Width = 0;
    int Height = 0;
    glfwGetFramebufferSize(Window, &Width, &Height);
    if (Width == 0 || Height == 0)
    {
        return false;
    }

    VkSurfaceCapabilitiesKHR Caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(Vk.PhysicalDevice, Vk.Surface, &Caps);

    uint32_t FormatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(Vk.PhysicalDevice, Vk.Surface, &FormatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> Formats(FormatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(Vk.PhysicalDevice, Vk.Surface, &FormatCount, Formats.data());
    VkSurfaceFormatKHR Format = Formats[0];
    for (const VkSurfaceFormatKHR& Candidate : Formats)
    {
        if (Candidate.format == VK_FORMAT_B8G8R8A8_UNORM && Candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            Format = Candidate;
        }
    }

    // Wayland reports 0xFFFFFFFF here and lets the client pick the size.
    VkExtent2D Extent = Caps.currentExtent;
    if (Extent.width == UINT32_MAX)
    {
        Extent.width = std::clamp(static_cast<uint32_t>(Width), Caps.minImageExtent.width, Caps.maxImageExtent.width);
        Extent.height = std::clamp(static_cast<uint32_t>(Height), Caps.minImageExtent.height, Caps.maxImageExtent.height);
    }

    uint32_t ImageCount = Caps.minImageCount + 1;
    if (Caps.maxImageCount != 0)
    {
        ImageCount = std::min(ImageCount, Caps.maxImageCount);
    }

    const VkSwapchainKHR OldSwapchain = Vk.Swapchain;
    const VkSwapchainCreateInfoKHR CreateInfo{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = Vk.Surface,
        .minImageCount = ImageCount,
        .imageFormat = Format.format,
        .imageColorSpace = Format.colorSpace,
        .imageExtent = Extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = Caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
        .oldSwapchain = OldSwapchain,
    };
    Check(vkCreateSwapchainKHR(Vk.Device, &CreateInfo, nullptr, &Vk.Swapchain), "vkCreateSwapchainKHR");

    DestroySwapchainResources(Vk);
    if (OldSwapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(Vk.Device, OldSwapchain, nullptr);
    }

    Vk.SwapchainFormat = Format.format;
    Vk.SwapchainExtent = Extent;
    vkGetSwapchainImagesKHR(Vk.Device, Vk.Swapchain, &ImageCount, nullptr);
    Vk.SwapchainImages.resize(ImageCount);
    vkGetSwapchainImagesKHR(Vk.Device, Vk.Swapchain, &ImageCount, Vk.SwapchainImages.data());

    for (const VkImage Image : Vk.SwapchainImages)
    {
        const VkImageViewCreateInfo ViewInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = Image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = Vk.SwapchainFormat,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        VkImageView View;
        Check(vkCreateImageView(Vk.Device, &ViewInfo, nullptr, &View), "vkCreateImageView");
        Vk.SwapchainViews.push_back(View);

        const VkSemaphoreCreateInfo SemaphoreInfo{ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkSemaphore Semaphore;
        Check(vkCreateSemaphore(Vk.Device, &SemaphoreInfo, nullptr, &Semaphore), "vkCreateSemaphore");
        Vk.RenderFinished.push_back(Semaphore);
    }
    return true;
}

void TransitionImage(const VkCommandBuffer Cmd, const VkImage Image, const VkImageLayout From, const VkImageLayout To)
{
    const VkImageMemoryBarrier2 Barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = From,
        .newLayout = To,
        .image = Image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    const VkDependencyInfo Dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &Barrier,
    };
    vkCmdPipelineBarrier2(Cmd, &Dependency);
}

// Returns false when the swapchain must be recreated.
bool DrawFrame(FVulkan& Vk)
{
    FFrame& Frame = Vk.Frames[Vk.FrameIndex];
    vkWaitForFences(Vk.Device, 1, &Frame.InFlight, VK_TRUE, UINT64_MAX);

    uint32_t ImageIndex = 0;
    const VkResult AcquireResult = vkAcquireNextImageKHR(Vk.Device, Vk.Swapchain, UINT64_MAX, Frame.ImageAcquired, VK_NULL_HANDLE, &ImageIndex);
    if (AcquireResult == VK_ERROR_OUT_OF_DATE_KHR)
    {
        return false;
    }
    if (AcquireResult != VK_SUBOPTIMAL_KHR)
    {
        Check(AcquireResult, "vkAcquireNextImageKHR");
    }
    vkResetFences(Vk.Device, 1, &Frame.InFlight);

    const VkCommandBuffer Cmd = Frame.CommandBuffer;
    vkResetCommandBuffer(Cmd, 0);
    const VkCommandBufferBeginInfo BeginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(Cmd, &BeginInfo);

    const VkImage Image = Vk.SwapchainImages[ImageIndex];
    TransitionImage(Cmd, Image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    const VkRenderingAttachmentInfo ColorAttachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = Vk.SwapchainViews[ImageIndex],
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = { .color = { { 0.05f, 0.02f, 0.08f, 1.0f } } },
    };
    const VkRenderingInfo RenderingInfo{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = { { 0, 0 }, Vk.SwapchainExtent },
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &ColorAttachment,
    };
    vkCmdBeginRendering(Cmd, &RenderingInfo);
    vkCmdEndRendering(Cmd);

    TransitionImage(Cmd, Image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    vkEndCommandBuffer(Cmd);

    const VkSemaphoreSubmitInfo WaitInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = Frame.ImageAcquired,
        .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    };
    const VkSemaphoreSubmitInfo SignalInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = Vk.RenderFinished[ImageIndex],
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    const VkCommandBufferSubmitInfo CmdInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = Cmd,
    };
    const VkSubmitInfo2 SubmitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = 1,
        .pWaitSemaphoreInfos = &WaitInfo,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &CmdInfo,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos = &SignalInfo,
    };
    Check(vkQueueSubmit2(Vk.Queue, 1, &SubmitInfo, Frame.InFlight), "vkQueueSubmit2");

    const VkPresentInfoKHR PresentInfo{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &Vk.RenderFinished[ImageIndex],
        .swapchainCount = 1,
        .pSwapchains = &Vk.Swapchain,
        .pImageIndices = &ImageIndex,
    };
    const VkResult PresentResult = vkQueuePresentKHR(Vk.Queue, &PresentInfo);
    Vk.FrameIndex = (Vk.FrameIndex + 1) % FramesInFlight;
    if (PresentResult == VK_ERROR_OUT_OF_DATE_KHR || PresentResult == VK_SUBOPTIMAL_KHR)
    {
        return false;
    }
    Check(PresentResult, "vkQueuePresentKHR");
    return true;
}

void Destroy(FVulkan& Vk)
{
    vkDeviceWaitIdle(Vk.Device);
    DestroySwapchainResources(Vk);
    vkDestroySwapchainKHR(Vk.Device, Vk.Swapchain, nullptr);
    for (const FFrame& Frame : Vk.Frames)
    {
        vkDestroySemaphore(Vk.Device, Frame.ImageAcquired, nullptr);
        vkDestroyFence(Vk.Device, Frame.InFlight, nullptr);
    }
    vkDestroyCommandPool(Vk.Device, Vk.CommandPool, nullptr);
    vkDestroyDevice(Vk.Device, nullptr);
    vkDestroySurfaceKHR(Vk.Instance, Vk.Surface, nullptr);
    vkDestroyInstance(Vk.Instance, nullptr);
}
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

int main(const int ArgCount, char** const Args)
{
    // The original data lives next to the original exe; pass another directory as an argument.
    std::filesystem::path DataRoot = "Game/Vyruz";
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
    GLFWwindow* const Window = glfwCreateWindow(OriginalWidth * 2, OriginalHeight * 2, "Vyruz", nullptr, nullptr);
    if (!Window)
    {
        std::fprintf(stderr, "glfwCreateWindow failed\n");
        return 1;
    }

    FVulkan Vk;
    CreateInstance(Vk);
    Check(glfwCreateWindowSurface(Vk.Instance, Window, nullptr, &Vk.Surface), "glfwCreateWindowSurface");
    CreateDevice(Vk);
    bool bSwapchainValid = CreateSwapchain(Vk, Window);

    while (!glfwWindowShouldClose(Window))
    {
        glfwPollEvents();
        if (!bSwapchainValid)
        {
            vkDeviceWaitIdle(Vk.Device);
            bSwapchainValid = CreateSwapchain(Vk, Window);
            if (!bSwapchainValid)
            {
                glfwWaitEvents();
            }
            continue;
        }
        bSwapchainValid = DrawFrame(Vk);
    }

    Destroy(Vk);
    glfwDestroyWindow(Window);
    glfwTerminate();
    return 0;
}
