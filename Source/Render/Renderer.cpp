#include "Render/Renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
#include "Sprite.vert.h"
#include "Sprite.frag.h"
#include "Blur.vert.h"
#include "Blur.frag.h"
#include "Aberration.vert.h"
#include "Aberration.frag.h"

// ponytail: fixed pool, one set per sprite sheet; grow or go bindless when levels load more sheets than this.
constexpr uint32_t MaxTextures = 1024;
// Each frame sits inside a border of copies of its own edge pixels: at a non-integer letterbox scale a pixel centre on a frame edge can sample one texel past it, which must not be a neighbour or a transparent gap.
constexpr uint32_t SheetBorder = 1;
constexpr uint32_t SheetPadding = 2 * SheetBorder;
// The blur halves the view four times, to a sixteenth, and comes back up; the spread widens each pass's taps.
constexpr uint32_t BlurLevelCount = 4;
constexpr float BlurSpread = 1.5f;

struct FBlurPush
{
    float HalfTexel[2] = {};
    float Up = 0.f;
    float Extract = 0.f;
    float Tint[4] = {};
};

// Shares the blur's pipeline layout, so it fills the same push range.
struct FAberrationPush
{
    float MaxOffset[2] = {};
    float Padding[6] = {};
};
static_assert(sizeof(FAberrationPush) == sizeof(FBlurPush));

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
    float Rect[4] = {};
    float UvRect[4] = {};
    float ScreenSize[2] = {};
    // A texel's size in UV for the outline, which looks at the neighbouring texels; zero draws the sprite as usual.
    float OutlineTexel[2] = {};
    // D3D's default texture stage modulates by the diffuse colour; a specular colour is added when present.
    float Diffuse[4] = {};
    float Specular[4] = {};
    // cos, sin, pivot x, pivot y.
    float Rotation[4] = { 1.f, 0.f, 0.f, 0.f };
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

bool HasDepth(const VkFormat Format)
{
    return Format != VK_FORMAT_S8_UINT;
}

void TransitionStencil(const VkCommandBuffer Cmd, const VkImage Image, const VkFormat Format)
{
    constexpr VkPipelineStageFlags2 TestStages = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    const VkImageMemoryBarrier2 Barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = TestStages,
        .srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dstStageMask = TestStages,
        .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .image = Image,
        .subresourceRange = { VkImageAspectFlags(VK_IMAGE_ASPECT_STENCIL_BIT | (HasDepth(Format) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0)), 0, 1, 0, 1 },
    };
    const VkDependencyInfo Dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &Barrier,
    };
    vkCmdPipelineBarrier2(Cmd, &Dependency);
}

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

float WideViewWidth(GLFWwindow* const Window)
{
    int Width = 0;
    int Height = 0;
    glfwGetFramebufferSize(Window, &Width, &Height);
    // Whole columns, so the view's pixels stay on the sprites' whole-pixel grid.
    return Height <= 0 ? ScreenWidth : std::max(ScreenWidth, std::floor(ScreenHeight * float(Width) / float(Height)));
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
    // Slang's SPIR-V declares draw parameters for the vertex index and demotes a discard to a helper invocation.
    VkPhysicalDeviceVulkan11Features Features11{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .shaderDrawParameters = VK_TRUE,
    };
    VkPhysicalDeviceVulkan13Features Features13{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext = &Features11,
        .shaderDemoteToHelperInvocation = VK_TRUE,
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

    // Linear filtering, which the sprite shader steers so the pixel art stays crisp at any scale.
    const VkSamplerCreateInfo SamplerInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
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

    // The blur has a sampler of its own: the NVIDIA driver 32.0.16.1742 crashed at startup with one immutable sampler in both set layouts.
    Check(vkCreateSampler(Device, &SamplerInfo, nullptr, &LinearSampler), "vkCreateSampler");
    VkDescriptorSetLayoutBinding BlurBinding = Binding;
    BlurBinding.pImmutableSamplers = &LinearSampler;
    const VkDescriptorSetLayoutCreateInfo BlurSetInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &BlurBinding,
    };
    Check(vkCreateDescriptorSetLayout(Device, &BlurSetInfo, nullptr, &BlurSetLayout), "vkCreateDescriptorSetLayout");
    // The chain's levels and the view's full-size copy.
    constexpr uint32_t BlurTargetCount = BlurLevelCount + 1;
    const VkDescriptorPoolSize BlurPoolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, BlurTargetCount };
    const VkDescriptorPoolCreateInfo BlurPoolInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = BlurTargetCount,
        .poolSizeCount = 1,
        .pPoolSizes = &BlurPoolSize,
    };
    Check(vkCreateDescriptorPool(Device, &BlurPoolInfo, nullptr, &BlurPool), "vkCreateDescriptorPool");
    const VkPushConstantRange BlurPushRange{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FBlurPush) };
    const VkPipelineLayoutCreateInfo BlurLayoutInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &BlurSetLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &BlurPushRange,
    };
    Check(vkCreatePipelineLayout(Device, &BlurLayoutInfo, nullptr, &BlurLayout), "vkCreatePipelineLayout");

    // The smallest stencil the device can draw to.
    for (const VkFormat Candidate : { VK_FORMAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D16_UNORM_S8_UINT })
    {
        VkFormatProperties Properties{};
        vkGetPhysicalDeviceFormatProperties(PhysicalDevice, Candidate, &Properties);
        if ((Properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0)
        {
            StencilFormat = Candidate;
            break;
        }
    }
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
    // Draws into the view set the stencil's test and operation per draw (EStencilMode); the blur's own chain has no stencil.
    const VkDynamicState DynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE };
    constexpr uint32_t ViewportStates = 2;
    const bool bStencil = StencilFormat != VK_FORMAT_UNDEFINED;
    const VkPipelineDynamicStateCreateInfo DynamicState{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = bStencil ? uint32_t(std::size(DynamicStates)) : ViewportStates,
        .pDynamicStates = DynamicStates,
    };
    const VkPipelineDynamicStateCreateInfo ChainDynamicState{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = ViewportStates,
        .pDynamicStates = DynamicStates,
    };
    constexpr uint32_t AllBits = 0xff;
    const VkStencilOpState StencilFace{ .compareMask = AllBits, .writeMask = AllBits };
    const VkPipelineDepthStencilStateCreateInfo DepthStencil{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .front = StencilFace,
        .back = StencilFace,
    };
    const VkPipelineRenderingCreateInfo RenderingInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &SwapchainFormat,
        .stencilAttachmentFormat = StencilFormat,
    };
    const VkPipelineRenderingCreateInfo ChainRenderingInfo{
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
        .pDepthStencilState = bStencil ? &DepthStencil : nullptr,
        .pColorBlendState = &ColorBlend,
        .pDynamicState = &DynamicState,
        .layout = PipelineLayout,
    };
    for (size_t Index = 0; Index < std::size(SpritePipelines); ++Index)
    {
        // Original hardware blend states at 00496280: mode 6 adds light, mode 8 darkens with inverse source RGB, modes 2 and 4 blend by alpha.
        const bool bAdditive = Index == static_cast<size_t>(ESpriteBlend::Additive);
        const bool bAlpha = Index == static_cast<size_t>(ESpriteBlend::Alpha);
        const bool bScreen = Index == static_cast<size_t>(ESpriteBlend::Screen);
        const bool bDouble = Index == static_cast<size_t>(ESpriteBlend::Double);
        const bool bOpaque = Index == static_cast<size_t>(ESpriteBlend::Opaque);
        // Opaque blends too, only to keep its colour as it is and clear the alpha.
        BlendAttachment.blendEnable = VK_TRUE;
        BlendAttachment.srcColorBlendFactor = bAdditive || bOpaque ? VK_BLEND_FACTOR_ONE
                                              : bAlpha  ? VK_BLEND_FACTOR_SRC_ALPHA
                                              : bScreen ? VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR
                                              : bDouble ? VK_BLEND_FACTOR_DST_COLOR
                                                        : VK_BLEND_FACTOR_ZERO;
        BlendAttachment.dstColorBlendFactor = bAdditive || bScreen || bDouble ? VK_BLEND_FACTOR_ONE
                                              : bAlpha             ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA
                                              : bOpaque            ? VK_BLEND_FACTOR_ZERO
                                                                   : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        // The view's alpha marks what glows for Bloom: an additive sprite sets it, an opaque one clears it, and one blended by alpha covers it by as much.
        BlendAttachment.srcAlphaBlendFactor = bAdditive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
        BlendAttachment.dstAlphaBlendFactor = bOpaque ? VK_BLEND_FACTOR_ZERO : bAlpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE;
        Check(vkCreateGraphicsPipelines(Device, VK_NULL_HANDLE, 1, &PipelineInfo, nullptr, &SpritePipelines[Index]), "vkCreateGraphicsPipelines");
    }

    vkDestroyShaderModule(Device, VertexModule, nullptr);
    vkDestroyShaderModule(Device, FragmentModule, nullptr);

    // The blur's passes draw one opaque triangle into its chain or the view, all in the swapchain's format.
    vkDestroyPipeline(Device, BlurPipeline, nullptr);
    vkDestroyPipeline(Device, BlurViewPipeline, nullptr);
    vkDestroyPipeline(Device, BloomPipeline, nullptr);
    const VkShaderModuleCreateInfo BlurVertexInfo{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(BlurVert),
        .pCode = BlurVert,
    };
    const VkShaderModuleCreateInfo BlurFragmentInfo{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(BlurFrag),
        .pCode = BlurFrag,
    };
    Check(vkCreateShaderModule(Device, &BlurVertexInfo, nullptr, &VertexModule), "vkCreateShaderModule");
    Check(vkCreateShaderModule(Device, &BlurFragmentInfo, nullptr, &FragmentModule), "vkCreateShaderModule");
    const VkPipelineShaderStageCreateInfo BlurStages[] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = VertexModule, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = FragmentModule, .pName = "main" },
    };
    const VkPipelineInputAssemblyStateCreateInfo TriangleList{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    BlendAttachment.blendEnable = VK_FALSE;
    VkGraphicsPipelineCreateInfo BlurInfo = PipelineInfo;
    BlurInfo.pStages = BlurStages;
    BlurInfo.pInputAssemblyState = &TriangleList;
    BlurInfo.layout = BlurLayout;
    Check(vkCreateGraphicsPipelines(Device, VK_NULL_HANDLE, 1, &BlurInfo, nullptr, &BlurViewPipeline), "vkCreateGraphicsPipelines");
    // Bloom adds its last pass onto the view's colour.
    BlendAttachment = {
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT,
    };
    Check(vkCreateGraphicsPipelines(Device, VK_NULL_HANDLE, 1, &BlurInfo, nullptr, &BloomPipeline), "vkCreateGraphicsPipelines");
    BlendAttachment.blendEnable = VK_FALSE;
    BlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    BlurInfo.pNext = &ChainRenderingInfo;
    BlurInfo.pDepthStencilState = nullptr;
    BlurInfo.pDynamicState = &ChainDynamicState;
    Check(vkCreateGraphicsPipelines(Device, VK_NULL_HANDLE, 1, &BlurInfo, nullptr, &BlurPipeline), "vkCreateGraphicsPipelines");
    vkDestroyShaderModule(Device, VertexModule, nullptr);
    vkDestroyShaderModule(Device, FragmentModule, nullptr);

    // The hit flash draws the view's copy back over it, opaque, through the blur's layout.
    vkDestroyPipeline(Device, AberrationPipeline, nullptr);
    const VkShaderModuleCreateInfo AberrationVertexInfo{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(AberrationVert),
        .pCode = AberrationVert,
    };
    const VkShaderModuleCreateInfo AberrationFragmentInfo{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(AberrationFrag),
        .pCode = AberrationFrag,
    };
    Check(vkCreateShaderModule(Device, &AberrationVertexInfo, nullptr, &VertexModule), "vkCreateShaderModule");
    Check(vkCreateShaderModule(Device, &AberrationFragmentInfo, nullptr, &FragmentModule), "vkCreateShaderModule");
    const VkPipelineShaderStageCreateInfo AberrationStages[] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = VertexModule, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = FragmentModule, .pName = "main" },
    };
    VkGraphicsPipelineCreateInfo AberrationInfo = PipelineInfo;
    AberrationInfo.pStages = AberrationStages;
    AberrationInfo.pInputAssemblyState = &TriangleList;
    AberrationInfo.layout = BlurLayout;
    Check(vkCreateGraphicsPipelines(Device, VK_NULL_HANDLE, 1, &AberrationInfo, nullptr, &AberrationPipeline), "vkCreateGraphicsPipelines");
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
    DestroyStencil();
}

void FRenderer::DestroyStencil()
{
    vkDestroyImageView(Device, StencilView, nullptr);
    vkDestroyImage(Device, StencilImage, nullptr);
    vkFreeMemory(Device, StencilMemory, nullptr);
    StencilView = VK_NULL_HANDLE;
    StencilImage = VK_NULL_HANDLE;
    StencilMemory = VK_NULL_HANDLE;
}

void FRenderer::CreateStencil()
{
    if (StencilFormat == VK_FORMAT_UNDEFINED)
    {
        return;
    }

    const VkImageCreateInfo ImageInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = StencilFormat,
        .extent = { SwapchainExtent.width, SwapchainExtent.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    Check(vkCreateImage(Device, &ImageInfo, nullptr, &StencilImage), "vkCreateImage");
    VkMemoryRequirements Requirements;
    vkGetImageMemoryRequirements(Device, StencilImage, &Requirements);
    const VkMemoryAllocateInfo Alloc{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = Requirements.size,
        .memoryTypeIndex = FindMemoryType(Requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    Check(vkAllocateMemory(Device, &Alloc, nullptr, &StencilMemory), "vkAllocateMemory");
    vkBindImageMemory(Device, StencilImage, StencilMemory, 0);
    const VkImageViewCreateInfo ViewInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = StencilImage,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = StencilFormat,
        .subresourceRange = { VkImageAspectFlags(VK_IMAGE_ASPECT_STENCIL_BIT | (HasDepth(StencilFormat) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0)), 0, 1, 0, 1 },
    };
    Check(vkCreateImageView(Device, &ViewInfo, nullptr, &StencilView), "vkCreateImageView");
}

void FRenderer::SetStencilMode(const EStencilMode Mode)
{
    if (StencilFormat != VK_FORMAT_UNDEFINED && Mode != StencilMode)
    {
        StencilMode = Mode;
        ApplyStencilMode();
    }
}

void FRenderer::ApplyStencilMode()
{
    if (StencilFormat == VK_FORMAT_UNDEFINED)
    {
        return;
    }

    const VkCommandBuffer Cmd = Frames[FrameIndex].CommandBuffer;
    constexpr uint32_t Marked = 1;
    constexpr uint32_t Unmarked = 0;
    const bool bOutline = StencilMode == EStencilMode::Outline;
    vkCmdSetStencilTestEnable(Cmd, StencilMode != EStencilMode::Off);
    vkCmdSetStencilOp(Cmd, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, bOutline ? VK_STENCIL_OP_KEEP : VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP,
        bOutline ? VK_COMPARE_OP_EQUAL : VK_COMPARE_OP_ALWAYS);
    vkCmdSetStencilReference(Cmd, VK_STENCIL_FACE_FRONT_AND_BACK, StencilMode == EStencilMode::ClearEntity ? Unmarked : Marked);
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
    bCanCapture = (Caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
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
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (bCanCapture ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0u),
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
    // The blur copies the view out of the swapchain image with a filtered blit and draws its chain in the same format.
    constexpr VkFormatFeatureFlags BlurFeatures = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                                  VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    VkFormatProperties FormatProperties{};
    vkGetPhysicalDeviceFormatProperties(PhysicalDevice, SwapchainFormat, &FormatProperties);
    bCanBlur = bCanCapture && (FormatProperties.optimalTilingFeatures & BlurFeatures) == BlurFeatures;
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

    CreateStencil();
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

// Staging copy through a one-off command buffer that waits for the queue: textures are uploaded while loading, and video frames 30 times a second.
void FRenderer::UploadTexture(const VkImage Image, const uint32_t Width, const uint32_t Height, const std::span<const uint32_t> Rgba, const VkImageLayout From)
{
    const VkDeviceSize ByteSize = Rgba.size_bytes();
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
    const bool bSampled = From == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    TransitionImage(Cmd, Image, From, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, bSampled ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE,
        bSampled ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkBufferImageCopy Region{
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { Width, Height, 1 },
    };
    vkCmdCopyBufferToImage(Cmd, Staging, Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &Region);
    TransitionImage(Cmd, Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
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

    UploadTexture(Texture.Image, Width, Height, Rgba, VK_IMAGE_LAYOUT_UNDEFINED);
    Texture.Width = Width;
    Texture.Height = Height;

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

FSpriteSheet FRenderer::CreateGradientSheet(const std::function<float(float)>& Brightness)
{
    constexpr uint32_t Size = 256;
    constexpr float Half = float(Size) * 0.5f;
    constexpr float ChannelMax = 255.f;
    constexpr uint32_t Opaque = 0xFFu << 24;
    std::vector<uint32_t> Rgba(size_t{ Size } * Size);
    for (uint32_t Y = 0; Y < Size; ++Y)
    {
        for (uint32_t X = 0; X < Size; ++X)
        {
            const float Distance = std::hypot((float(X) + 0.5f - Half) / Half, (float(Y) + 0.5f - Half) / Half);
            const uint32_t Level = uint32_t(std::lround(std::clamp(Brightness(Distance), 0.f, 1.f) * ChannelMax));
            Rgba[size_t{ Y } * Size + X] = Level | Level << 8 | Level << 16 | Opaque;
        }
    }

    FSpriteSheet Sheet;
    Sheet.Texture = CreateTexture(Size, Size, Rgba);
    Sheet.Frames.push_back({ .U0 = 0.f, .V0 = 0.f, .U1 = 1.f, .V1 = 1.f, .Width = Size, .Height = Size });
    return Sheet;
}

FSpriteSheet FRenderer::CreateVideoSheet(const uint32_t Width, const uint32_t Height)
{
    FSpriteSheet Sheet;
    Sheet.Texture = CreateTexture(Width, Height, std::vector<uint32_t>(size_t{ Width } * Height, 0));
    Sheet.Frames.push_back({ .U0 = 0.f, .V0 = 0.f, .U1 = 1.f, .V1 = 1.f, .Width = Width, .Height = Height });
    return Sheet;
}

void FRenderer::UpdateVideoSheet(const FSpriteSheet& Sheet, const std::span<const uint32_t> Rgba)
{
    if (Sheet.Texture >= Textures.size())
    {
        return;
    }

    const FTexture& Texture = Textures[Sheet.Texture];
    if (Rgba.size() != size_t{ Texture.Width } * Texture.Height)
    {
        return;
    }

    // ponytail: waits for the frames in flight to let go of the texture; a texture per frame in flight if this ever shows.
    vkDeviceWaitIdle(Device);
    UploadTexture(Texture.Image, Texture.Width, Texture.Height, Rgba, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void FRenderer::SetViewShift(const float X, const float Y)
{
    ShiftX = X;
    ShiftY = Y;
}

bool FRenderer::BeginFrame(const float Width)
{
    ViewWidth = Width;
    ShiftX = ShiftY = 0.f;
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
    if (StencilImage != VK_NULL_HANDLE)
    {
        TransitionStencil(Cmd, StencilImage, StencilFormat);
    }

    StencilMode = EStencilMode::Off;
    BeginRendering(VK_ATTACHMENT_LOAD_OP_CLEAR);
    return true;
}

void FRenderer::BeginRendering(const VkAttachmentLoadOp LoadOp)
{
    const VkCommandBuffer Cmd = Frames[FrameIndex].CommandBuffer;
    const VkRenderingAttachmentInfo ColorAttachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = SwapchainViews[ImageIndex],
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = LoadOp,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = { .color = { { 0.0f, 0.0f, 0.0f, 0.0f } } },
    };
    // The stencil starts each frame clear and carries over when the rendering resumes.
    const VkRenderingAttachmentInfo StencilAttachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = StencilView,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .loadOp = LoadOp,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = { .depthStencil = { .depth = 1.f, .stencil = 0 } },
    };
    const VkRenderingInfo RenderingInfo{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = { { 0, 0 }, SwapchainExtent },
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &ColorAttachment,
        .pStencilAttachment = StencilView != VK_NULL_HANDLE ? &StencilAttachment : nullptr,
    };
    vkCmdBeginRendering(Cmd, &RenderingInfo);
    ApplyStencilMode();

    // Letterbox the view into the window; bars stay at the clear color.
    const float Scale = std::min(SwapchainExtent.width / ViewWidth, SwapchainExtent.height / ScreenHeight);
    const float ScaledWidth = ViewWidth * Scale;
    const float ScaledHeight = ScreenHeight * Scale;
    const VkViewport Viewport{
        .x = (SwapchainExtent.width - ScaledWidth) * 0.5f,
        .y = (SwapchainExtent.height - ScaledHeight) * 0.5f,
        .width = ScaledWidth,
        .height = ScaledHeight,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    const VkRect2D Scissor{
        { static_cast<int32_t>(Viewport.x), static_cast<int32_t>(Viewport.y) },
        { static_cast<uint32_t>(ScaledWidth), static_cast<uint32_t>(ScaledHeight) },
    };
    vkCmdSetViewport(Cmd, 0, 1, &Viewport);
    vkCmdSetScissor(Cmd, 0, 1, &Scissor);
}

void FRenderer::DrawSprite(const FSpriteSheet& Sheet, const size_t FrameIndexInSheet, const float CellX, const float CellY, const bool bMirrorX, const bool bMirrorY,
    const ESpriteBlend Blend, const float ScaleX, const float ScaleY, const uint32_t VisibleColumns, const uint32_t Diffuse, const uint32_t Specular,
    const float Alpha, const float RotationDegrees, const float CellPivotX, const float CellPivotY)
{
    const float X = CellX + ShiftX;
    const float Y = CellY + ShiftY;
    const float PivotX = CellPivotX + ShiftX;
    const float PivotY = CellPivotY + ShiftY;
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
    // A turned frame may reach anywhere within its farthest corner's distance of the pivot.
    const float Reach = RotationDegrees != 0.f ? std::max(std::hypot(Left - PivotX, Top - PivotY), std::hypot(Left + Width - PivotX, Top + Height - PivotY)) : 0.f;
    if (RotationDegrees != 0.f ? PivotX - Reach >= ViewWidth || PivotY - Reach >= ScreenHeight || PivotX + Reach <= 0.f || PivotY + Reach <= 0.f
                               : Left >= ViewWidth || Top >= ScreenHeight || Left + Width <= 0.f || Top + Height <= 0.f)
    {
        return;
    }

    FSpritePush Push{
        .Rect = { Left, Top, Width, Height },
        .UvRect = { bMirrorX ? U1 : Frame.U0, bMirrorY ? Frame.V1 : Frame.V0, bMirrorX ? Frame.U0 : U1, bMirrorY ? Frame.V0 : Frame.V1 },
        .ScreenSize = { ViewWidth, ScreenHeight },
    };
    UnpackColor(Diffuse, Push.Diffuse, Alpha);
    UnpackColor(Specular, Push.Specular);
    if (RotationDegrees != 0.f)
    {
        constexpr double RadiansPerDegree = 3.14159265358979323846 / 180.;
        const double Radians = double(RotationDegrees) * RadiansPerDegree;
        Push.Rotation[0] = float(std::cos(Radians));
        Push.Rotation[1] = float(std::sin(Radians));
        Push.Rotation[2] = PivotX;
        Push.Rotation[3] = PivotY;
    }
    const bool bOutline = StencilMode == EStencilMode::Outline;
    if (bOutline)
    {
        const FTexture& Texture = Textures[Sheet.Texture];
        Push.OutlineTexel[0] = 1.f / float(std::max(1u, Texture.Width));
        Push.OutlineTexel[1] = 1.f / float(std::max(1u, Texture.Height));
    }

    vkCmdBindPipeline(Cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, SpritePipelines[static_cast<size_t>(bOutline ? ESpriteBlend::Alpha : Blend)]);
    vkCmdBindDescriptorSets(Cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, PipelineLayout, 0, 1, &Textures[Sheet.Texture].DescriptorSet, 0, nullptr);
    vkCmdPushConstants(Cmd, PipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push), &Push);
    vkCmdDraw(Cmd, 4, 1, 0, 0);
}

void FRenderer::SetGamma(const std::array<int32_t, 3>& Ramp)
{
    constexpr int32_t Limit = 0xff;
    for (size_t Channel = 0; Channel < Gamma.size(); ++Channel)
    {
        Gamma[Channel] = std::clamp(Ramp[Channel], -Limit, Limit);
    }
}

void FRenderer::SetUserGamma(const std::array<int32_t, 3>& Ramp)
{
    SetGamma(Ramp);
    UserGamma = Gamma;
}

void FRenderer::ResetGamma()
{
    Gamma = UserGamma;
}

const std::array<int32_t, 3>& FRenderer::GetGamma() const
{
    return Gamma;
}

const std::array<int32_t, 3>& FRenderer::GetUserGamma() const
{
    return UserGamma;
}

std::string FRenderer::GetDeviceName() const
{
    VkPhysicalDeviceProperties Properties{};
    vkGetPhysicalDeviceProperties(PhysicalDevice, &Properties);
    return Properties.deviceName;
}

// The swapchain is UNORM, so the blends work on display values as the ramp did: a screen blend lifts, an inverse-source blend darkens.
void FRenderer::DrawGamma()
{
    const auto Pack = [&](const auto Part) { return uint32_t(Part(Gamma[0])) << 16 | uint32_t(Part(Gamma[1])) << 8 | uint32_t(Part(Gamma[2])); };
    static constexpr int32_t ChannelMax = 0xff;
    const uint32_t Lift = Pack([](const int32_t Value) { return std::max(Value, 0); });
    const uint32_t Darken = Pack([](const int32_t Value) { return Value < 0 ? std::min(1 - Value, ChannelMax) : 0; });
    if (Lift == 0 && Darken == 0)
    {
        return;
    }

    if (Lift != 0)
    {
        DrawSprite(WhitePixel(), 0, 0.f, 0.f, false, false, ESpriteBlend::Screen, ViewWidth, ScreenHeight, UINT32_MAX, Lift);
    }

    if (Darken != 0)
    {
        DrawSprite(WhitePixel(), 0, 0.f, 0.f, false, false, ESpriteBlend::InverseSourceColor, ViewWidth, ScreenHeight, UINT32_MAX, Darken);
    }
}

void FRenderer::DoubleRect(const float X, const float Y, const float Width, const float Height)
{
    DrawSprite(WhitePixel(), 0, X, Y, false, false, ESpriteBlend::Double, Width, Height);
}

const FSpriteSheet& FRenderer::WhitePixel()
{
    if (!GammaSheet)
    {
        constexpr uint16_t White = 0x7fff;
        GammaSheet = CreateSpriteSheet({ Gaf::FFrame{ .Width = 1, .Height = 1, .Pixels = { White } } });
    }

    return *GammaSheet;
}

void FRenderer::DestroyBlurTargets()
{
    if (ViewCopy.Image != VK_NULL_HANDLE)
    {
        BlurLevels.push_back(ViewCopy);
        ViewCopy = {};
    }

    for (const FTexture& Level : BlurLevels)
    {
        vkDestroyImageView(Device, Level.View, nullptr);
        vkDestroyImage(Device, Level.Image, nullptr);
        vkFreeMemory(Device, Level.Memory, nullptr);
    }

    BlurLevels.clear();
    Check(vkResetDescriptorPool(Device, BlurPool, 0), "vkResetDescriptorPool");
}

void FRenderer::CreateBlurTargets(const VkExtent2D View)
{
    // The previous frame may still read the old chain.
    vkDeviceWaitIdle(Device);
    DestroyBlurTargets();
    const auto MakeTarget = [&](const uint32_t Width, const uint32_t Height)
    {
        FTexture Level{ .Width = std::max(1u, Width), .Height = std::max(1u, Height) };
        const VkImageCreateInfo ImageInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = SwapchainFormat,
            .extent = { Level.Width, Level.Height, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        Check(vkCreateImage(Device, &ImageInfo, nullptr, &Level.Image), "vkCreateImage");
        VkMemoryRequirements Requirements;
        vkGetImageMemoryRequirements(Device, Level.Image, &Requirements);
        const VkMemoryAllocateInfo Alloc{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = Requirements.size,
            .memoryTypeIndex = FindMemoryType(Requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
        };
        Check(vkAllocateMemory(Device, &Alloc, nullptr, &Level.Memory), "vkAllocateMemory");
        vkBindImageMemory(Device, Level.Image, Level.Memory, 0);
        const VkImageViewCreateInfo ViewInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = Level.Image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = SwapchainFormat,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        Check(vkCreateImageView(Device, &ViewInfo, nullptr, &Level.View), "vkCreateImageView");
        const VkDescriptorSetAllocateInfo SetAlloc{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = BlurPool,
            .descriptorSetCount = 1,
            .pSetLayouts = &BlurSetLayout,
        };
        Check(vkAllocateDescriptorSets(Device, &SetAlloc, &Level.DescriptorSet), "vkAllocateDescriptorSets");
        const VkDescriptorImageInfo DescriptorImage{
            .imageView = Level.View,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        const VkWriteDescriptorSet Write{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = Level.DescriptorSet,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &DescriptorImage,
        };
        vkUpdateDescriptorSets(Device, 1, &Write, 0, nullptr);
        return Level;
    };

    // The chain starts at half the view.
    for (uint32_t Index = 1; Index <= BlurLevelCount; ++Index)
    {
        BlurLevels.push_back(MakeTarget(View.width >> Index, View.height >> Index));
    }

    ViewCopy = MakeTarget(View.width, View.height);
}

FRenderer::FPlacedView FRenderer::PlaceView() const
{
    const float Scale = std::min(SwapchainExtent.width / ViewWidth, SwapchainExtent.height / ScreenHeight);
    return {
        .Left = std::max(0, int32_t((SwapchainExtent.width - ViewWidth * Scale) * 0.5f)),
        .Top = std::max(0, int32_t((SwapchainExtent.height - ScreenHeight * Scale) * 0.5f)),
        .Size = { std::max(1u, uint32_t(ViewWidth * Scale)), std::max(1u, uint32_t(ScreenHeight * Scale)) },
    };
}

void FRenderer::EnsureViewTargets(const VkExtent2D View)
{
    if (ViewCopy.Width != View.width || ViewCopy.Height != View.height)
    {
        CreateBlurTargets(View);
    }
}

void FRenderer::DrawBlurPass(const FTexture& From, const bool bUp, const uint32_t Tint, const VkPipeline Pipeline, const bool bExtract)
{
    const VkCommandBuffer Cmd = Frames[FrameIndex].CommandBuffer;
    FBlurPush Push{
        .HalfTexel = { BlurSpread * 0.5f / float(From.Width), BlurSpread * 0.5f / float(From.Height) },
        .Up = bUp ? 1.f : 0.f,
        .Extract = bExtract ? 1.f : 0.f,
    };
    UnpackColor(Tint, Push.Tint);
    vkCmdBindPipeline(Cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, Pipeline);
    vkCmdBindDescriptorSets(Cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, BlurLayout, 0, 1, &From.DescriptorSet, 0, nullptr);
    vkCmdPushConstants(Cmd, BlurLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push), &Push);
    vkCmdDraw(Cmd, 3, 1, 0, 0);
}

void FRenderer::BlurView(const uint32_t Tint)
{
    SetStencilMode(EStencilMode::Off);
    constexpr uint32_t ChannelMask = 0xffffff;
    if (!bCanBlur)
    {
        // An inverse-source blend of the tint's complement scales what lies under it by the tint.
        DrawSprite(WhitePixel(), 0, 0.f, 0.f, false, false, ESpriteBlend::InverseSourceColor, ViewWidth, ScreenHeight, UINT32_MAX, ~Tint & ChannelMask);
        return;
    }

    RunBlurChain(false);
    DrawBlurPass(BlurLevels.front(), true, Tint & ChannelMask, BlurViewPipeline);
}

void FRenderer::Bloom()
{
    if (!bCanBlur)
    {
        return;
    }

    // How much of the blurred glow comes back over the view, per channel.
    constexpr uint32_t BloomStrength = 0x383838;
    SetStencilMode(EStencilMode::Off);
    RunBlurChain(true);
    DrawBlurPass(BlurLevels.front(), true, BloomStrength, BloomPipeline);
}

void FRenderer::Aberrate(const float Strength)
{
    if (!bCanBlur || Strength <= 0.f)
    {
        return;
    }

    // How far red and blue part at the view's edges at full strength, in screen pixels of the original 480 rows.
    constexpr float MaxSplit = 5.f;
    SetStencilMode(EStencilMode::Off);
    const FPlacedView Placed = PlaceView();
    EnsureViewTargets(Placed.Size);
    const VkCommandBuffer Cmd = Frames[FrameIndex].CommandBuffer;
    const VkImage Image = SwapchainImages[ImageIndex];
    vkCmdEndRendering(Cmd);
    TransitionImage(Cmd, Image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    TransitionImage(Cmd, ViewCopy.Image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkImageCopy Copy{
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffset = { Placed.Left, Placed.Top, 0 },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .extent = { std::min(Placed.Size.width, SwapchainExtent.width - uint32_t(Placed.Left)), std::min(Placed.Size.height, SwapchainExtent.height - uint32_t(Placed.Top)), 1 },
    };
    vkCmdCopyImage(Cmd, Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ViewCopy.Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &Copy);
    TransitionImage(Cmd, ViewCopy.Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    TransitionImage(Cmd, Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
        VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    BeginRendering(VK_ATTACHMENT_LOAD_OP_LOAD);
    const float Split = std::clamp(Strength, 0.f, 1.f) * MaxSplit;
    const FAberrationPush Push{ .MaxOffset = { Split / ViewWidth, Split / ScreenHeight } };
    vkCmdBindPipeline(Cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, AberrationPipeline);
    vkCmdBindDescriptorSets(Cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, BlurLayout, 0, 1, &ViewCopy.DescriptorSet, 0, nullptr);
    vkCmdPushConstants(Cmd, BlurLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push), &Push);
    vkCmdDraw(Cmd, 3, 1, 0, 0);
}

void FRenderer::RunBlurChain(const bool bGlow)
{
    const FPlacedView Placed = PlaceView();
    EnsureViewTargets(Placed.Size);
    const int32_t Left = Placed.Left;
    const int32_t Top = Placed.Top;
    const VkExtent2D View = Placed.Size;

    const VkCommandBuffer Cmd = Frames[FrameIndex].CommandBuffer;
    const VkImage Image = SwapchainImages[ImageIndex];
    vkCmdEndRendering(Cmd);
    // The first halving is a filtered blit of the view.
    const FTexture& First = BlurLevels.front();
    TransitionImage(Cmd, Image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    TransitionImage(Cmd, First.Image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkImageBlit Blit{
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets = { { std::max(0, Left), std::max(0, Top), 0 }, { std::max(0, Left) + int32_t(View.width), std::max(0, Top) + int32_t(View.height), 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets = { { 0, 0, 0 }, { int32_t(First.Width), int32_t(First.Height), 1 } },
    };
    vkCmdBlitImage(Cmd, Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, First.Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &Blit, VK_FILTER_LINEAR);
    TransitionImage(Cmd, First.Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    TransitionImage(Cmd, Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
        VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    constexpr uint32_t White = 0xffffff;
    const auto Pass = [&](const FTexture& From, const FTexture& To, const bool bUp, const bool bExtract)
    {
        TransitionImage(Cmd, To.Image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        const VkRenderingAttachmentInfo Attachment{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = To.View,
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        };
        const VkRenderingInfo Rendering{
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = { { 0, 0 }, { To.Width, To.Height } },
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &Attachment,
        };
        vkCmdBeginRendering(Cmd, &Rendering);
        const VkViewport Viewport{ .width = float(To.Width), .height = float(To.Height), .maxDepth = 1.f };
        const VkRect2D Scissor{ { 0, 0 }, { To.Width, To.Height } };
        vkCmdSetViewport(Cmd, 0, 1, &Viewport);
        vkCmdSetScissor(Cmd, 0, 1, &Scissor);
        DrawBlurPass(From, bUp, White, BlurPipeline, bExtract);
        vkCmdEndRendering(Cmd);
        TransitionImage(Cmd, To.Image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    };
    // A glow keeps only what the view's alpha marks, from the first pass down.
    for (size_t Index = 1; Index < BlurLevels.size(); ++Index)
    {
        Pass(BlurLevels[Index - 1], BlurLevels[Index], false, bGlow && Index == 1);
    }

    for (size_t Index = BlurLevels.size() - 1; Index-- > 0;)
    {
        Pass(BlurLevels[Index + 1], BlurLevels[Index], true, false);
    }

    // The caller's last doubling lands in the view.
    BeginRendering(VK_ATTACHMENT_LOAD_OP_LOAD);
}

void FRenderer::RequestCapture(const uint32_t Width, const uint32_t Height)
{
    CaptureSize = VkExtent2D{ Width, Height };
}

std::optional<std::vector<uint16_t>> FRenderer::TakeCapture()
{
    if (CaptureBuffer == VK_NULL_HANDLE || !CaptureSize)
    {
        return std::nullopt;
    }

    vkQueueWaitIdle(Queue);
    const VkExtent2D Size = *CaptureSize;
    const VkExtent2D Region = CaptureRegion;
    const bool bBgra = SwapchainFormat == VK_FORMAT_B8G8R8A8_UNORM || SwapchainFormat == VK_FORMAT_B8G8R8A8_SRGB;
    void* Mapped = nullptr;
    vkMapMemory(Device, CaptureMemory, 0, VK_WHOLE_SIZE, 0, &Mapped);
    const auto* const Source = static_cast<const uint8_t*>(Mapped);
    std::vector<uint16_t> Pixels(size_t(Size.width) * Size.height);
    // Each pixel averages the block of the view it covers.
    constexpr uint32_t ChannelShift = 3;
    constexpr uint32_t RedShift = 10;
    constexpr uint32_t GreenShift = 5;
    for (uint32_t Y = 0; Y < Size.height; ++Y)
    {
        const uint32_t Top = Y * Region.height / Size.height;
        const uint32_t Bottom = std::max(Top + 1, (Y + 1) * Region.height / Size.height);
        for (uint32_t X = 0; X < Size.width; ++X)
        {
            const uint32_t Left = X * Region.width / Size.width;
            const uint32_t Right = std::max(Left + 1, (X + 1) * Region.width / Size.width);
            std::array<uint32_t, 3> Sum{};
            for (uint32_t Row = Top; Row < Bottom; ++Row)
            {
                for (uint32_t Column = Left; Column < Right; ++Column)
                {
                    const uint8_t* const Pixel = Source + (size_t(Row) * Region.width + Column) * 4;
                    Sum[0] += Pixel[bBgra ? 2 : 0];
                    Sum[1] += Pixel[1];
                    Sum[2] += Pixel[bBgra ? 0 : 2];
                }
            }

            const uint32_t Count = (Bottom - Top) * (Right - Left);
            Pixels[size_t(Y) * Size.width + X] =
                uint16_t((Sum[0] / Count >> ChannelShift) << RedShift | (Sum[1] / Count >> ChannelShift) << GreenShift | (Sum[2] / Count >> ChannelShift));
        }
    }

    vkUnmapMemory(Device, CaptureMemory);
    vkDestroyBuffer(Device, CaptureBuffer, nullptr);
    vkFreeMemory(Device, CaptureMemory, nullptr);
    CaptureBuffer = VK_NULL_HANDLE;
    CaptureMemory = VK_NULL_HANDLE;
    CaptureSize.reset();
    return Pixels;
}

void FRenderer::EndFrame()
{
    FFrame& Frame = Frames[FrameIndex];
    const VkCommandBuffer Cmd = Frame.CommandBuffer;
    const VkImage Image = SwapchainImages[ImageIndex];
    if (CaptureSize && bCanCapture && CaptureBuffer == VK_NULL_HANDLE)
    {
        vkCmdEndRendering(Cmd);
        // The middle 640x480 of the letterboxed view, as BeginFrame placed it.
        const float Scale = std::min(SwapchainExtent.width / ViewWidth, SwapchainExtent.height / ScreenHeight);
        CaptureRegion = { std::max(1u, uint32_t(ScreenWidth * Scale)), std::max(1u, uint32_t(ScreenHeight * Scale)) };
        const int32_t OffsetX = int32_t((SwapchainExtent.width - ScreenWidth * Scale) * 0.5f);
        const int32_t OffsetY = int32_t((SwapchainExtent.height - ScreenHeight * Scale) * 0.5f);
        const VkDeviceSize ByteSize = VkDeviceSize(CaptureRegion.width) * CaptureRegion.height * 4;
        const VkBufferCreateInfo BufferInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = ByteSize,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        Check(vkCreateBuffer(Device, &BufferInfo, nullptr, &CaptureBuffer), "vkCreateBuffer");
        VkMemoryRequirements Requirements;
        vkGetBufferMemoryRequirements(Device, CaptureBuffer, &Requirements);
        const VkMemoryAllocateInfo Alloc{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = Requirements.size,
            .memoryTypeIndex = FindMemoryType(Requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
        };
        Check(vkAllocateMemory(Device, &Alloc, nullptr, &CaptureMemory), "vkAllocateMemory");
        vkBindBufferMemory(Device, CaptureBuffer, CaptureMemory, 0);
        TransitionImage(Cmd, Image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        const VkBufferImageCopy Region{
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageOffset = { std::max(0, OffsetX), std::max(0, OffsetY), 0 },
            .imageExtent = { CaptureRegion.width, CaptureRegion.height, 1 },
        };
        vkCmdCopyImageToBuffer(Cmd, Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, CaptureBuffer, 1, &Region);
        TransitionImage(Cmd, Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        BeginRendering(VK_ATTACHMENT_LOAD_OP_LOAD);
    }

    SetStencilMode(EStencilMode::Off);
    DrawGamma();
    vkCmdEndRendering(Cmd);
    TransitionImage(Cmd, Image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);

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
    GammaSheet.reset();
    Check(vkResetDescriptorPool(Device, DescriptorPool, 0), "vkResetDescriptorPool");
}

void FRenderer::Shutdown()
{
    ReleaseSpriteSheets();
    DestroyBlurTargets();
    for (const VkPipeline Pipeline : SpritePipelines)
    {
        vkDestroyPipeline(Device, Pipeline, nullptr);
    }

    vkDestroyPipeline(Device, BlurPipeline, nullptr);
    vkDestroyPipeline(Device, AberrationPipeline, nullptr);
    vkDestroyPipeline(Device, BloomPipeline, nullptr);
    vkDestroyPipeline(Device, BlurViewPipeline, nullptr);
    vkDestroyPipelineLayout(Device, BlurLayout, nullptr);
    vkDestroyDescriptorPool(Device, BlurPool, nullptr);
    vkDestroyDescriptorSetLayout(Device, BlurSetLayout, nullptr);
    vkDestroySampler(Device, LinearSampler, nullptr);

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
