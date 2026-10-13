#include "Render/Renderer.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
#include "Sprite.vert.h"
#include "Sprite.frag.h"

// ponytail: fixed pool, one set per sprite sheet; grow or go bindless when levels load more sheets than this.
constexpr uint32_t MaxTextures = 1024;
// Each frame sits inside a border of copies of its own edge pixels: at a non-integer letterbox scale a pixel centre on a frame edge can sample one texel past it, which must not be a neighbour or a transparent gap.
constexpr uint32_t SheetBorder = 1;
constexpr uint32_t SheetPadding = 2 * SheetBorder;

void Check(const VkResult Result, const char* const What)
{
    if (Result != VK_SUCCESS)
    {
        std::fprintf(stderr, "%s failed: %d\n", What, static_cast<int>(Result));
        std::exit(1);
    }
}

struct FSpritePush
{
    float Rect[4];
    float UvRect[4];
    float ScreenSize[2];
    float Padding[2];
    // D3D's default texture stage modulates by the diffuse colour; a specular colour is added when present.
    float Diffuse[4];
    float Specular[4];
};

// Unpacks a D3DCOLOR's red, green and blue bytes; the fourth channel carries the sprite's alpha.
void UnpackColor(const uint32_t Color, float (&Out)[4], const float Alpha = 1.f)
{
    constexpr float ChannelMax = 255.f;
    constexpr int RedShift = 16;
    constexpr int GreenShift = 8;
    constexpr uint32_t ChannelMask = 0xff;
    Out[0] = float((Color >> RedShift) & ChannelMask) / ChannelMax;
    Out[1] = float((Color >> GreenShift) & ChannelMask) / ChannelMax;
    Out[2] = float(Color & ChannelMask) / ChannelMax;
    Out[3] = Alpha;
}

// Expands 5-bit channels to 8 bits the same way most D3D drivers did: replicate the top bits into the low bits.
constexpr uint32_t Rgb555ToRgba8(const uint16_t Pixel)
{
    const uint32_t R = (Pixel >> 10) & 0x1F;
    const uint32_t G = (Pixel >> 5) & 0x1F;
    const uint32_t B = Pixel & 0x1F;
    return (R << 3 | R >> 2) | (G << 3 | G >> 2) << 8 | (B << 3 | B >> 2) << 16 | 0xFFu << 24;
}
static_assert(Rgb555ToRgba8(0x7FFF) == 0xFFFFFFFF);
static_assert(Rgb555ToRgba8(0x7C00) == 0xFF0000FF);

void TransitionImage(const VkCommandBuffer Cmd, const VkImage Image, const VkImageLayout From, const VkImageLayout To,
    const VkPipelineStageFlags2 SrcStage, const VkAccessFlags2 SrcAccess, const VkPipelineStageFlags2 DstStage, const VkAccessFlags2 DstAccess)
{
    const VkImageMemoryBarrier2 Barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = SrcStage,
        .srcAccessMask = SrcAccess,
        .dstStageMask = DstStage,
        .dstAccessMask = DstAccess,
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
}

bool FRenderer::Init(GLFWwindow* const InWindow)
{
    Window = InWindow;
    CreateInstance();
    Check(glfwCreateWindowSurface(Instance, Window, nullptr, &Surface), "glfwCreateWindowSurface");
    CreateDevice();
    bSwapchainValid = CreateSwapchain();
    return true;
}

void FRenderer::CreateInstance()
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
    Check(vkCreateInstance(&CreateInfo, nullptr, &Instance), "vkCreateInstance");
}

void FRenderer::CreateDevice()
{
    uint32_t DeviceCount = 0;
    vkEnumeratePhysicalDevices(Instance, &DeviceCount, nullptr);
    std::vector<VkPhysicalDevice> Devices(DeviceCount);
    vkEnumeratePhysicalDevices(Instance, &DeviceCount, Devices.data());

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
            vkGetPhysicalDeviceSurfaceSupportKHR(Candidate, Family, Surface, &CanPresent);
            if (!(Families[Family].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !CanPresent)
            {
                continue;
            }

            const int Score = Properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 1 : 0;
            if (Score > BestScore)
            {
                BestScore = Score;
                PhysicalDevice = Candidate;
                QueueFamily = Family;
            }
            break;
        }
    }
    if (PhysicalDevice == VK_NULL_HANDLE)
    {
        std::fprintf(stderr, "No Vulkan 1.3 device with graphics and present support\n");
        std::exit(1);
    }

    const float Priority = 1.0f;
    const VkDeviceQueueCreateInfo QueueInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = QueueFamily,
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
    Check(vkCreateDevice(PhysicalDevice, &CreateInfo, nullptr, &Device), "vkCreateDevice");
    vkGetDeviceQueue(Device, QueueFamily, 0, &Queue);

    const VkCommandPoolCreateInfo PoolInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = QueueFamily,
    };
    Check(vkCreateCommandPool(Device, &PoolInfo, nullptr, &CommandPool), "vkCreateCommandPool");

    for (FFrame& Frame : Frames)
    {
        const VkCommandBufferAllocateInfo AllocInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = CommandPool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        Check(vkAllocateCommandBuffers(Device, &AllocInfo, &Frame.CommandBuffer), "vkAllocateCommandBuffers");

        const VkSemaphoreCreateInfo SemaphoreInfo{ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        Check(vkCreateSemaphore(Device, &SemaphoreInfo, nullptr, &Frame.ImageAcquired), "vkCreateSemaphore");

        const VkFenceCreateInfo FenceInfo{ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
        Check(vkCreateFence(Device, &FenceInfo, nullptr, &Frame.InFlight), "vkCreateFence");
    }

    // Nearest filtering keeps the original pixel art crisp when scaled up.
    const VkSamplerCreateInfo SamplerInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    Check(vkCreateSampler(Device, &SamplerInfo, nullptr, &Sampler), "vkCreateSampler");

    const VkDescriptorSetLayoutBinding Binding{
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .pImmutableSamplers = &Sampler,
    };
    const VkDescriptorSetLayoutCreateInfo SetLayoutInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &Binding,
    };
    Check(vkCreateDescriptorSetLayout(Device, &SetLayoutInfo, nullptr, &DescriptorSetLayout), "vkCreateDescriptorSetLayout");

    const VkDescriptorPoolSize PoolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MaxTextures };
    const VkDescriptorPoolCreateInfo DescriptorPoolInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = MaxTextures,
        .poolSizeCount = 1,
        .pPoolSizes = &PoolSize,
    };
    Check(vkCreateDescriptorPool(Device, &DescriptorPoolInfo, nullptr, &DescriptorPool), "vkCreateDescriptorPool");

    const VkPushConstantRange PushRange{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FSpritePush) };
    const VkPipelineLayoutCreateInfo PipelineLayoutInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &DescriptorSetLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &PushRange,
    };
    Check(vkCreatePipelineLayout(Device, &PipelineLayoutInfo, nullptr, &PipelineLayout), "vkCreatePipelineLayout");
}

void FRenderer::CreatePipeline()
{
    for (const VkPipeline Pipeline : SpritePipelines)
    {
        vkDestroyPipeline(Device, Pipeline, nullptr);
    }

    VkShaderModule VertexModule;
    VkShaderModule FragmentModule;
    const VkShaderModuleCreateInfo VertexInfo{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(SpriteVert),
        .pCode = SpriteVert,
    };
    const VkShaderModuleCreateInfo FragmentInfo{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(SpriteFrag),
        .pCode = SpriteFrag,
    };
    Check(vkCreateShaderModule(Device, &VertexInfo, nullptr, &VertexModule), "vkCreateShaderModule");
    Check(vkCreateShaderModule(Device, &FragmentInfo, nullptr, &FragmentModule), "vkCreateShaderModule");

    // slangc renames every entry point to "main" in the emitted SPIR-V.
    const VkPipelineShaderStageCreateInfo Stages[] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = VertexModule, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = FragmentModule, .pName = "main" },
    };
    const VkPipelineVertexInputStateCreateInfo VertexInput{ .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    const VkPipelineInputAssemblyStateCreateInfo InputAssembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
    };
    const VkPipelineViewportStateCreateInfo ViewportState{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    const VkPipelineRasterizationStateCreateInfo Rasterization{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .lineWidth = 1.0f,
    };
    const VkPipelineMultisampleStateCreateInfo Multisample{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState BlendAttachment{
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    const VkPipelineColorBlendStateCreateInfo ColorBlend{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &BlendAttachment,
    };
    const VkDynamicState DynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    const VkPipelineDynamicStateCreateInfo DynamicState{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = DynamicStates,
    };
    const VkPipelineRenderingCreateInfo RenderingInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &SwapchainFormat,
    };
    const VkGraphicsPipelineCreateInfo PipelineInfo{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &RenderingInfo,
        .stageCount = 2,
        .pStages = Stages,
        .pVertexInputState = &VertexInput,
        .pInputAssemblyState = &InputAssembly,
        .pViewportState = &ViewportState,
        .pRasterizationState = &Rasterization,
        .pMultisampleState = &Multisample,
        .pColorBlendState = &ColorBlend,
        .pDynamicState = &DynamicState,
        .layout = PipelineLayout,
    };
    for (size_t Index = 0; Index < std::size(SpritePipelines); ++Index)
    {
        // Original hardware blend states at 00496280: mode 6 adds light, mode 8 darkens with inverse source RGB, modes 2 and 4 blend by alpha.
        const bool bAdditive = Index == static_cast<size_t>(ESpriteBlend::Additive);
        const bool bAlpha = Index == static_cast<size_t>(ESpriteBlend::Alpha);
        BlendAttachment.blendEnable = Index != static_cast<size_t>(ESpriteBlend::Opaque);
        BlendAttachment.srcColorBlendFactor = bAdditive ? VK_BLEND_FACTOR_ONE : bAlpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_ZERO;
        BlendAttachment.dstColorBlendFactor = bAdditive ? VK_BLEND_FACTOR_ONE : bAlpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        BlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        BlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        Check(vkCreateGraphicsPipelines(Device, VK_NULL_HANDLE, 1, &PipelineInfo, nullptr, &SpritePipelines[Index]), "vkCreateGraphicsPipelines");
    }

    vkDestroyShaderModule(Device, VertexModule, nullptr);
    vkDestroyShaderModule(Device, FragmentModule, nullptr);
    PipelineFormat = SwapchainFormat;
}

void FRenderer::DestroySwapchainResources()
{
    for (const VkImageView View : SwapchainViews)
    {
        vkDestroyImageView(Device, View, nullptr);
    }
    for (const VkSemaphore Semaphore : RenderFinished)
    {
        vkDestroySemaphore(Device, Semaphore, nullptr);
    }
    SwapchainViews.clear();
    RenderFinished.clear();
}

bool FRenderer::CreateSwapchain()
{
    int Width = 0;
    int Height = 0;
    glfwGetFramebufferSize(Window, &Width, &Height);
    if (Width == 0 || Height == 0)
    {
        return false;
    }

    VkSurfaceCapabilitiesKHR Caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(PhysicalDevice, Surface, &Caps);

    uint32_t FormatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(PhysicalDevice, Surface, &FormatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> Formats(FormatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(PhysicalDevice, Surface, &FormatCount, Formats.data());
    // UNORM, not SRGB: the original wrote 16-bit colors straight to the framebuffer with no gamma conversion.
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

    const VkSwapchainKHR OldSwapchain = Swapchain;
    const VkSwapchainCreateInfoKHR CreateInfo{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = Surface,
        .minImageCount = ImageCount,
        .imageFormat = Format.format,
        .imageColorSpace = Format.colorSpace,
        .imageExtent = Extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = Caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
        .oldSwapchain = OldSwapchain,
    };
    Check(vkCreateSwapchainKHR(Device, &CreateInfo, nullptr, &Swapchain), "vkCreateSwapchainKHR");

    DestroySwapchainResources();
    if (OldSwapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(Device, OldSwapchain, nullptr);
    }

    SwapchainFormat = Format.format;
    SwapchainExtent = Extent;
    vkGetSwapchainImagesKHR(Device, Swapchain, &ImageCount, nullptr);
    SwapchainImages.resize(ImageCount);
    vkGetSwapchainImagesKHR(Device, Swapchain, &ImageCount, SwapchainImages.data());

    for (const VkImage Image : SwapchainImages)
    {
        const VkImageViewCreateInfo ViewInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = Image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = SwapchainFormat,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        VkImageView View;
        Check(vkCreateImageView(Device, &ViewInfo, nullptr, &View), "vkCreateImageView");
        SwapchainViews.push_back(View);

        const VkSemaphoreCreateInfo SemaphoreInfo{ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkSemaphore Semaphore;
        Check(vkCreateSemaphore(Device, &SemaphoreInfo, nullptr, &Semaphore), "vkCreateSemaphore");
        RenderFinished.push_back(Semaphore);
    }

    if (PipelineFormat != SwapchainFormat)
    {
        CreatePipeline();
    }
    return true;
}

uint32_t FRenderer::FindMemoryType(const uint32_t TypeBits, const VkMemoryPropertyFlags Properties) const
{
    VkPhysicalDeviceMemoryProperties MemoryProperties;
    vkGetPhysicalDeviceMemoryProperties(PhysicalDevice, &MemoryProperties);
    for (uint32_t Index = 0; Index < MemoryProperties.memoryTypeCount; ++Index)
    {
        if ((TypeBits & (1u << Index)) && (MemoryProperties.memoryTypes[Index].propertyFlags & Properties) == Properties)
        {
            return Index;
        }
    }
    std::fprintf(stderr, "No suitable Vulkan memory type\n");
    std::exit(1);
}

uint32_t FRenderer::CreateTexture(const uint32_t Width, const uint32_t Height, const std::vector<uint32_t>& Rgba)
{
    if (Textures.size() >= MaxTextures)
    {
        std::fprintf(stderr, "Texture limit (%u) reached\n", MaxTextures);
        std::exit(1);
    }

    FTexture Texture;
    const VkImageCreateInfo ImageInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { Width, Height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    Check(vkCreateImage(Device, &ImageInfo, nullptr, &Texture.Image), "vkCreateImage");

    VkMemoryRequirements ImageRequirements;
    vkGetImageMemoryRequirements(Device, Texture.Image, &ImageRequirements);
    const VkMemoryAllocateInfo ImageAlloc{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = ImageRequirements.size,
        .memoryTypeIndex = FindMemoryType(ImageRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    Check(vkAllocateMemory(Device, &ImageAlloc, nullptr, &Texture.Memory), "vkAllocateMemory");
    vkBindImageMemory(Device, Texture.Image, Texture.Memory, 0);

    // Staging copy through a one-off command buffer; texture uploads only happen while loading.
    const VkDeviceSize ByteSize = Rgba.size() * sizeof(uint32_t);
    VkBuffer Staging;
    VkDeviceMemory StagingMemory;
    const VkBufferCreateInfo BufferInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = ByteSize,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    };
    Check(vkCreateBuffer(Device, &BufferInfo, nullptr, &Staging), "vkCreateBuffer");
    VkMemoryRequirements BufferRequirements;
    vkGetBufferMemoryRequirements(Device, Staging, &BufferRequirements);
    const VkMemoryAllocateInfo BufferAlloc{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = BufferRequirements.size,
        .memoryTypeIndex = FindMemoryType(BufferRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    Check(vkAllocateMemory(Device, &BufferAlloc, nullptr, &StagingMemory), "vkAllocateMemory");
    vkBindBufferMemory(Device, Staging, StagingMemory, 0);
    void* Mapped = nullptr;
    vkMapMemory(Device, StagingMemory, 0, ByteSize, 0, &Mapped);
    std::memcpy(Mapped, Rgba.data(), ByteSize);
    vkUnmapMemory(Device, StagingMemory);

    VkCommandBuffer Cmd;
    const VkCommandBufferAllocateInfo CmdAlloc{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = CommandPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    Check(vkAllocateCommandBuffers(Device, &CmdAlloc, &Cmd), "vkAllocateCommandBuffers");
    const VkCommandBufferBeginInfo BeginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(Cmd, &BeginInfo);
    TransitionImage(Cmd, Texture.Image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkBufferImageCopy Region{
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { Width, Height, 1 },
    };
    vkCmdCopyBufferToImage(Cmd, Staging, Texture.Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &Region);
    TransitionImage(Cmd, Texture.Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    vkEndCommandBuffer(Cmd);
    const VkSubmitInfo SubmitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &Cmd,
    };
    Check(vkQueueSubmit(Queue, 1, &SubmitInfo, VK_NULL_HANDLE), "vkQueueSubmit");
    vkQueueWaitIdle(Queue);
    vkFreeCommandBuffers(Device, CommandPool, 1, &Cmd);
    vkDestroyBuffer(Device, Staging, nullptr);
    vkFreeMemory(Device, StagingMemory, nullptr);

    const VkImageViewCreateInfo ViewInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = Texture.Image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    Check(vkCreateImageView(Device, &ViewInfo, nullptr, &Texture.View), "vkCreateImageView");

    const VkDescriptorSetAllocateInfo SetAlloc{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = DescriptorPool,
        .descriptorSetCount = 1,
        .pSetLayouts = &DescriptorSetLayout,
    };
    Check(vkAllocateDescriptorSets(Device, &SetAlloc, &Texture.DescriptorSet), "vkAllocateDescriptorSets");
    const VkDescriptorImageInfo DescriptorImage{
        .imageView = Texture.View,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    const VkWriteDescriptorSet Write{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = Texture.DescriptorSet,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &DescriptorImage,
    };
    vkUpdateDescriptorSets(Device, 1, &Write, 0, nullptr);

    Textures.push_back(Texture);
    return static_cast<uint32_t>(Textures.size() - 1);
}

FSpriteSheet FRenderer::CreateSpriteSheet(const std::vector<Gaf::FFrame>& Frames)
{
    FSpriteSheet Sheet;
    if (Frames.empty())
    {
        return Sheet;
    }

    // Shelf packing: frames left to right, wrapping into a new row when the sheet width is exceeded.
    uint32_t SheetWidth = 512;
    for (const Gaf::FFrame& Frame : Frames)
    {
        SheetWidth = std::max(SheetWidth, Frame.Width + SheetPadding);
    }
    struct FPlacement
    {
        uint32_t X;
        uint32_t Y;
    };
    std::vector<FPlacement> Placements;
    uint32_t CursorX = 0;
    uint32_t CursorY = 0;
    uint32_t RowHeight = 0;
    for (const Gaf::FFrame& Frame : Frames)
    {
        if (CursorX + Frame.Width + SheetPadding > SheetWidth)
        {
            CursorX = 0;
            CursorY += RowHeight + SheetPadding;
            RowHeight = 0;
        }
        Placements.push_back({ CursorX, CursorY });
        CursorX += Frame.Width + SheetPadding;
        RowHeight = std::max(RowHeight, Frame.Height);
    }
    const uint32_t SheetHeight = CursorY + RowHeight + SheetPadding;

    std::vector<uint32_t> Rgba(size_t{ SheetWidth } * SheetHeight, 0);
    for (size_t Index = 0; Index < Frames.size(); ++Index)
    {
        const Gaf::FFrame& Frame = Frames[Index];
        const FPlacement Place = { .X = Placements[Index].X + SheetBorder, .Y = Placements[Index].Y + SheetBorder };
        for (uint32_t Y = 0; Frame.Width != 0 && Frame.Height != 0 && Y < Frame.Height + SheetPadding; ++Y)
        {
            for (uint32_t X = 0; X < Frame.Width + SheetPadding; ++X)
            {
                const uint32_t SourceX = std::clamp(X, SheetBorder, Frame.Width + SheetBorder - 1) - SheetBorder;
                const uint32_t SourceY = std::clamp(Y, SheetBorder, Frame.Height + SheetBorder - 1) - SheetBorder;
                const uint16_t Pixel = Frame.Pixels[size_t{ SourceY } * Frame.Width + SourceX];
                Rgba[size_t{ Placements[Index].Y + Y } * SheetWidth + Placements[Index].X + X] = Pixel == Frame.ColorKey ? 0 : Rgb555ToRgba8(Pixel);
            }
        }
        Sheet.Frames.push_back({
            .U0 = float(Place.X) / SheetWidth,
            .V0 = float(Place.Y) / SheetHeight,
            .U1 = float(Place.X + Frame.Width) / SheetWidth,
            .V1 = float(Place.Y + Frame.Height) / SheetHeight,
            .Width = Frame.Width,
            .Height = Frame.Height,
            .TrimX = Frame.TrimX,
            .TrimY = Frame.TrimY,
        });
    }
    Sheet.Texture = CreateTexture(SheetWidth, SheetHeight, Rgba);
    return Sheet;
}

bool FRenderer::BeginFrame()
{
    if (!bSwapchainValid)
    {
        vkDeviceWaitIdle(Device);
        bSwapchainValid = CreateSwapchain();
        if (!bSwapchainValid)
        {
            return false;
        }
    }

    FFrame& Frame = Frames[FrameIndex];
    vkWaitForFences(Device, 1, &Frame.InFlight, VK_TRUE, UINT64_MAX);

    const VkResult AcquireResult = vkAcquireNextImageKHR(Device, Swapchain, UINT64_MAX, Frame.ImageAcquired, VK_NULL_HANDLE, &ImageIndex);
    if (AcquireResult == VK_ERROR_OUT_OF_DATE_KHR)
    {
        bSwapchainValid = false;
        return false;
    }
    if (AcquireResult != VK_SUBOPTIMAL_KHR)
    {
        Check(AcquireResult, "vkAcquireNextImageKHR");
    }
    vkResetFences(Device, 1, &Frame.InFlight);

    const VkCommandBuffer Cmd = Frame.CommandBuffer;
    vkResetCommandBuffer(Cmd, 0);
    const VkCommandBufferBeginInfo BeginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(Cmd, &BeginInfo);
    TransitionImage(Cmd, SwapchainImages[ImageIndex], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    const VkRenderingAttachmentInfo ColorAttachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = SwapchainViews[ImageIndex],
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = { .color = { { 0.0f, 0.0f, 0.0f, 1.0f } } },
    };
    const VkRenderingInfo RenderingInfo{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = { { 0, 0 }, SwapchainExtent },
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &ColorAttachment,
    };
    vkCmdBeginRendering(Cmd, &RenderingInfo);

    // Letterbox the 4:3 original screen into the window; bars stay at the clear color.
    const float Scale = std::min(SwapchainExtent.width / ScreenWidth, SwapchainExtent.height / ScreenHeight);
    const float ViewWidth = ScreenWidth * Scale;
    const float ViewHeight = ScreenHeight * Scale;
    const VkViewport Viewport{
        .x = (SwapchainExtent.width - ViewWidth) * 0.5f,
        .y = (SwapchainExtent.height - ViewHeight) * 0.5f,
        .width = ViewWidth,
        .height = ViewHeight,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    const VkRect2D Scissor{
        { static_cast<int32_t>(Viewport.x), static_cast<int32_t>(Viewport.y) },
        { static_cast<uint32_t>(ViewWidth), static_cast<uint32_t>(ViewHeight) },
    };
    vkCmdSetViewport(Cmd, 0, 1, &Viewport);
    vkCmdSetScissor(Cmd, 0, 1, &Scissor);
    return true;
}

void FRenderer::DrawSprite(const FSpriteSheet& Sheet, const size_t FrameIndexInSheet, const float X, const float Y, const bool bMirrorX, const bool bMirrorY,
    const ESpriteBlend Blend, const float ScaleX, const float ScaleY, const uint32_t VisibleColumns, const uint32_t Diffuse, const uint32_t Specular,
    const float Alpha)
{
    if (Sheet.Texture >= Textures.size() || FrameIndexInSheet >= Sheet.Frames.size())
    {
        return;
    }
    const VkCommandBuffer Cmd = Frames[FrameIndex].CommandBuffer;
    const FSpriteFrame& Frame = Sheet.Frames[FrameIndexInSheet];
    // DrawGobFrame at 00466f40 mirrors around the sprite origin, including the cropped bitmap offset.
    // ponytail: scales the trimmed bitmap around the cell origin; DrawGobFrame truncates the scaled cell size and trim to whole pixels.
    const uint32_t Columns = std::min(Frame.Width, VisibleColumns);
    if (Columns == 0)
    {
        return;
    }

    const float Width = float(Columns) * ScaleX;
    const float U1 = Frame.U0 + (Frame.U1 - Frame.U0) * float(Columns) / float(Frame.Width);
    const float Height = float(Frame.Height) * ScaleY;
    const float Left = X + (bMirrorX ? -float(Frame.TrimX) * ScaleX - Width : float(Frame.TrimX) * ScaleX);
    const float Top = Y + (bMirrorY ? -float(Frame.TrimY) * ScaleY - Height : float(Frame.TrimY) * ScaleY);
    if (Left >= ScreenWidth || Top >= ScreenHeight || Left + Width <= 0.f || Top + Height <= 0.f)
    {
        return;
    }

    FSpritePush Push{
        .Rect = { Left, Top, Width, Height },
        .UvRect = { bMirrorX ? U1 : Frame.U0, bMirrorY ? Frame.V1 : Frame.V0, bMirrorX ? Frame.U0 : U1, bMirrorY ? Frame.V0 : Frame.V1 },
        .ScreenSize = { ScreenWidth, ScreenHeight },
    };
    UnpackColor(Diffuse, Push.Diffuse, Alpha);
    UnpackColor(Specular, Push.Specular);
    vkCmdBindPipeline(Cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, SpritePipelines[static_cast<size_t>(Blend)]);
    vkCmdBindDescriptorSets(Cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, PipelineLayout, 0, 1, &Textures[Sheet.Texture].DescriptorSet, 0, nullptr);
    vkCmdPushConstants(Cmd, PipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push), &Push);
    vkCmdDraw(Cmd, 4, 1, 0, 0);
}

void FRenderer::EndFrame()
{
    FFrame& Frame = Frames[FrameIndex];
    const VkCommandBuffer Cmd = Frame.CommandBuffer;
    vkCmdEndRendering(Cmd);
    TransitionImage(Cmd, SwapchainImages[ImageIndex], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
    vkEndCommandBuffer(Cmd);

    const VkSemaphoreSubmitInfo WaitInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = Frame.ImageAcquired,
        .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    };
    const VkSemaphoreSubmitInfo SignalInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = RenderFinished[ImageIndex],
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
    Check(vkQueueSubmit2(Queue, 1, &SubmitInfo, Frame.InFlight), "vkQueueSubmit2");

    const VkPresentInfoKHR PresentInfo{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &RenderFinished[ImageIndex],
        .swapchainCount = 1,
        .pSwapchains = &Swapchain,
        .pImageIndices = &ImageIndex,
    };
    const VkResult PresentResult = vkQueuePresentKHR(Queue, &PresentInfo);
    FrameIndex = (FrameIndex + 1) % FramesInFlight;
    if (PresentResult == VK_ERROR_OUT_OF_DATE_KHR || PresentResult == VK_SUBOPTIMAL_KHR)
    {
        bSwapchainValid = false;
        return;
    }
    Check(PresentResult, "vkQueuePresentKHR");
}

void FRenderer::ReleaseSpriteSheets()
{
    vkDeviceWaitIdle(Device);
    for (const FTexture& Texture : Textures)
    {
        vkDestroyImageView(Device, Texture.View, nullptr);
        vkDestroyImage(Device, Texture.Image, nullptr);
        vkFreeMemory(Device, Texture.Memory, nullptr);
    }
    Textures.clear();
    Check(vkResetDescriptorPool(Device, DescriptorPool, 0), "vkResetDescriptorPool");
}

void FRenderer::Shutdown()
{
    ReleaseSpriteSheets();
    for (const VkPipeline Pipeline : SpritePipelines)
    {
        vkDestroyPipeline(Device, Pipeline, nullptr);
    }

    vkDestroyPipelineLayout(Device, PipelineLayout, nullptr);
    vkDestroyDescriptorPool(Device, DescriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(Device, DescriptorSetLayout, nullptr);
    vkDestroySampler(Device, Sampler, nullptr);
    DestroySwapchainResources();
    vkDestroySwapchainKHR(Device, Swapchain, nullptr);
    for (const FFrame& Frame : Frames)
    {
        vkDestroySemaphore(Device, Frame.ImageAcquired, nullptr);
        vkDestroyFence(Device, Frame.InFlight, nullptr);
    }
    vkDestroyCommandPool(Device, CommandPool, nullptr);
    vkDestroyDevice(Device, nullptr);
    vkDestroySurfaceKHR(Instance, Surface, nullptr);
    vkDestroyInstance(Instance, nullptr);
}
