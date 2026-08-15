/// @file GBAStationVulkan.cpp
/// @brief Vulkan frontend implementation.
///
/// Lifecycle:
///   CreateInstance()                  // VkInstance + VkSurface
///   retro_set_environment(...)        // core registers SET_HW_RENDER + neg iface
///   retro_init()
///   retro_load_game()                 // core does its non-GPU init
///   CreateDeviceAndSwapchain()        // negotiation->create_device + swapchain
///   hw_render_callback.context_reset()// core calls GET_HW_RENDER_INTERFACE on us
///   loop:
///     BeginFrame() -> retro_run() -> EndFrame()
///
/// The libretro Vulkan interface is sparsely documented; the canonical
/// reference is core/deps/libretro-common/include/libretro_vulkan.h.

#include "GBAStationVulkan.h"
#include "GBAStationLogger.h"
#include "GBAStationSlangPreset.h"

#include "imgui.h"
#include "imgui_impl_vulkan.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <vector>

// Static ICD entry points exposed by Mesa's loaderless libvulkan.a (NVK).
// Same shape dolphin uses on Switch — there's no shared libvulkan to dlopen.
#if defined(__SWITCH__)
extern "C" {
PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char* pName);
VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* pVersion);
}

static VKAPI_ATTR VkResult VKAPI_CALL GBAStationEnumerateInstanceLayerProperties(
    uint32_t* pPropertyCount, VkLayerProperties* /*pProperties*/)
{
    if (!pPropertyCount)
        return VK_ERROR_INITIALIZATION_FAILED;
    *pPropertyCount = 0;
    return VK_SUCCESS;
}

static VkInstance g_GBAStationIcdInstance = VK_NULL_HANDLE;

static void GBAStationIcdTrace(const char* name, VkInstance instance, PFN_vkVoidFunction function)
{
    FILE* fp = fopen("sdmc:/GBAStation/debug/flycast_stub.log", "ab");
    if (!fp)
        return;
    std::fprintf(fp, "[VK] gipa name=%s instance=%p function=%p\\n", name,
                 static_cast<void*>(instance), reinterpret_cast<void*>(function));
    std::fclose(fp);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GBAStationGetDeviceProcAddr(
    VkDevice /*device*/, const char* pName)
{
    if (!pName)
        return nullptr;
    PFN_vkVoidFunction func = vk_icdGetInstanceProcAddr(g_GBAStationIcdInstance, pName);
    if (!func)
        func = vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, pName);
    return func;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GBAStationGetInstanceProcAddr(
    VkInstance instance, const char* pName)
{
    if (!pName)
        return nullptr;
    if (instance != VK_NULL_HANDLE)
        g_GBAStationIcdInstance = instance;
    if (std::strcmp(pName, "vkGetInstanceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&GBAStationGetInstanceProcAddr);
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&GBAStationGetDeviceProcAddr);
    if (std::strcmp(pName, "vkEnumerateInstanceLayerProperties") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&GBAStationEnumerateInstanceLayerProperties);
    const PFN_vkVoidFunction function = vk_icdGetInstanceProcAddr(instance, pName);
    if (std::strcmp(pName, "vkEnumeratePhysicalDevices") == 0 ||
        std::strcmp(pName, "vkGetPhysicalDeviceProperties") == 0) {
        GBAStationIcdTrace(pName, instance, function);
    }
    return function;
}
#endif

// Note: VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE is defined in
// core/rend/vulkan/vk_context_lr.cpp; we share the same dispatcher.

namespace GBAStationVulkan
{
namespace
{

constexpr const char* TAG = "VK";
constexpr uint32_t kGBAStationImageExtentMagic = 0x47425845; // "GBXE"

// Must match the private pNext payload in vk_context_lr.cpp.  This avoids
// blitting a scaled core framebuffer with its unrelated logical AV extent.
struct GBAStationImageExtent
{
    uint32_t magic;
    uint32_t width;
    uint32_t height;
};

// --- State ---------------------------------------------------------------

struct PerFrame
{
    vk::CommandBuffer cmd;
    vk::Fence inflightFence;        // signalled when GPU finishes this frame
    vk::Semaphore acquireSemaphore; // signalled by vkAcquireNextImage
    vk::Semaphore renderSemaphore;  // signalled by our submit, waited by present
    uint32_t presentedImage = 0;    // swap image index presented by this frame

    // Set by core via set_image() each frame. Pointer ownership stays with
    // the core; we read it from EndFrame.
    retro_vulkan_image image{};
    bool imageValid = false;

    // Optional command buffers the core asked us to submit (set_command_buffers).
    std::vector<vk::CommandBuffer> coreCommandBuffers;

    // Optional semaphore the core wants signalled when we finish using the image.
    vk::Semaphore signalSemaphore = VK_NULL_HANDLE;

    // Staging allocations recorded into this frame's command buffer.  They
    // must outlive the GPU copy and are reclaimed only when this slot's fence
    // is signalled on its next use.
    std::vector<vk::Buffer> deferredUploadBuffers;
    std::vector<vk::DeviceMemory> deferredUploadMemories;
    vk::Buffer thumbnailBuffer;
    vk::DeviceMemory thumbnailMemory;
    uint32_t thumbnailWidth = 0;
    uint32_t thumbnailHeight = 0;
    bool thumbnailPending = false;
};
struct OverlayTextureResource
{
    vk::Image image;
    vk::DeviceMemory memory;
    vk::ImageView view;
    vk::Sampler sampler;
    VkDescriptorSet descriptor = VK_NULL_HANDLE;
};

struct PendingOverlayUpload
{
    vk::Buffer stagingBuffer;
    vk::DeviceMemory stagingMemory;
    vk::Image image;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct SlangTarget
{
    vk::Image image;
    vk::DeviceMemory memory;
    vk::ImageView view;
    vk::Framebuffer framebuffer;
    vk::Extent2D extent{};
    bool initialized = false;
};

struct SlangPassRuntime
{
    const GBAStationSlang::Pass* definition = nullptr;
    vk::DescriptorSetLayout descriptorSetLayout;
    vk::PipelineLayout pipelineLayout;
    vk::Pipeline pipeline;
    std::vector<vk::DescriptorSet> descriptorSets;
    std::vector<vk::Buffer> uniformBuffers;
    std::vector<vk::DeviceMemory> uniformMemories;
    std::vector<SlangTarget> targets;
    vk::Extent2D targetExtent{};
    uint32_t pushConstantSize = 0;
    uint32_t uniformSize = 0;
};

struct SlangRuntime
{
    const GBAStationSlang::Preset* preset = nullptr;
    uint64_t presetVersion = 0;
    vk::RenderPass renderPass;
    vk::DescriptorPool descriptorPool;
    vk::Sampler nearestSampler;
    vk::Sampler linearSampler;
    vk::Buffer vertexBuffer;
    vk::DeviceMemory vertexMemory;
    std::vector<SlangPassRuntime> passes;
    vk::Extent2D sourceExtent{};
    vk::Extent2D viewportExtent{};
    uint32_t frameCount = 0;
    uint32_t diagnosticFrames = 0;
};

struct SlangOutput
{
    vk::Image image;
    vk::Extent2D extent{};
    bool valid = false;
};

vk::Instance        s_instance;
vk::PhysicalDevice  s_gpu;
vk::Device          s_device;
vk::Queue           s_queue;
vk::Queue           s_presentQueue;
uint32_t            s_queueFamilyIndex = 0;

vk::SurfaceKHR      s_surface;
vk::SwapchainKHR    s_swapchain;
vk::Format          s_swapFormat = vk::Format::eUndefined;
vk::Extent2D        s_swapExtent;
vk::Extent2D        s_sourceExtent;  // last frame extent from video_refresh
vk::Rect2D          s_gameViewport{{0, 0}, {0, 0}};  // game blit dst; 0 extent => full swap
std::vector<vk::Image>     s_swapImages;
std::vector<vk::ImageView> s_swapImageViews;
vk::RenderPass     s_overlayRenderPass;
std::vector<vk::Framebuffer> s_overlayFramebuffers;

vk::CommandPool     s_commandPool;
std::vector<PerFrame> s_frames;
uint32_t            s_currentFrame = 0;   // rotating frame slot (== libretro sync_index)
uint32_t            s_currentImage = 0;   // current swap image returned by acquire
bool                s_frameInFlight = false;
bool                s_thumbnailCaptureRequested = false;
int                 s_thumbnailCaptureFrame = -1;
bool                s_ready = false;
bool                s_overlayReady = false;
vk::DescriptorPool s_overlayDescriptorPool;
ImDrawData*        s_overlayDrawData = nullptr;
retro_vulkan_image s_lastImage{};
bool               s_lastImageValid = false;
uint32_t           s_lastImageFrame = 0;
std::vector<OverlayTextureResource> s_overlayTextures;
std::vector<PendingOverlayUpload> s_pendingOverlayUploads;
std::vector<OverlayTextureResource> s_retiredOverlayTextures;
const GBAStationSlang::Preset* s_activeSlangPreset = nullptr;
uint64_t s_activeSlangPresetVersion = 0;
std::vector<SlangRuntime> s_slangRuntimes;
std::string s_slangLastFailure;
bool s_slangCleanupRequested = false;
vk::Extent2D s_slangKeepSourceExtent{};
vk::Extent2D s_slangKeepViewportExtent{};

const retro_hw_render_context_negotiation_interface_vulkan* s_negIface = nullptr;
retro_hw_render_interface_vulkan s_hwIface{};

std::mutex s_queueMutex;

#ifdef __SWITCH__
NWindow* s_nwindow = nullptr;
#endif

// --- Logging -------------------------------------------------------------

#define VK_LOG_INFO(fmt, ...)  LOG_INFO(TAG, fmt, ##__VA_ARGS__)
#define VK_LOG_WARN(fmt, ...)  LOG_WARN(TAG, fmt, ##__VA_ARGS__)
#define VK_LOG_ERROR(fmt, ...) LOG_ERROR(TAG, fmt, ##__VA_ARGS__)

// --- Helpers -------------------------------------------------------------

void TransitionLayout(vk::CommandBuffer cmd, vk::Image image,
                      vk::ImageLayout oldLayout, vk::ImageLayout newLayout,
                      vk::AccessFlags srcAccess, vk::AccessFlags dstAccess,
                      vk::PipelineStageFlags srcStage, vk::PipelineStageFlags dstStage)
{
    vk::ImageMemoryBarrier barrier;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    cmd.pipelineBarrier(srcStage, dstStage, vk::DependencyFlags(), nullptr, nullptr, barrier);
}

PFN_vkGetInstanceProcAddr GetInstanceProcAddrFunc()
{
#if defined(__SWITCH__)
    setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);
    setenv("MESA_SHADER_CACHE_DISABLE", "1", 1);
    uint32_t icdVersion = 5;
    vk_icdNegotiateLoaderICDInterfaceVersion(&icdVersion);
    VK_LOG_INFO("switchVK ICD negotiated version=%u", icdVersion);
    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(&GBAStationGetInstanceProcAddr);
#else
    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr);
#endif
}

PFN_vkVoidFunction ImGuiVulkanLoader(const char* functionName, void* /*userData*/)
{
    PFN_vkGetInstanceProcAddr getInstProcAddr = GetInstanceProcAddrFunc();
    if (!getInstProcAddr)
        return nullptr;
    return getInstProcAddr(static_cast<VkInstance>(s_instance), functionName);
}

bool CreateOverlayRenderTargets()
{
    try
    {
        vk::AttachmentDescription colorAttachment;
        colorAttachment.format = s_swapFormat;
        colorAttachment.samples = vk::SampleCountFlagBits::e1;
        colorAttachment.loadOp = vk::AttachmentLoadOp::eLoad;
        colorAttachment.storeOp = vk::AttachmentStoreOp::eStore;
        colorAttachment.stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
        colorAttachment.stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
        colorAttachment.initialLayout = vk::ImageLayout::eColorAttachmentOptimal;
        colorAttachment.finalLayout = vk::ImageLayout::eColorAttachmentOptimal;

        vk::AttachmentReference colorRef;
        colorRef.attachment = 0;
        colorRef.layout = vk::ImageLayout::eColorAttachmentOptimal;

        vk::SubpassDescription subpass;
        subpass.pipelineBindPoint = vk::PipelineBindPoint::eGraphics;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;

        vk::RenderPassCreateInfo rpci;
        rpci.attachmentCount = 1;
        rpci.pAttachments = &colorAttachment;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &subpass;
        s_overlayRenderPass = s_device.createRenderPass(rpci);

        s_overlayFramebuffers.resize(s_swapImageViews.size());
        for (size_t i = 0; i < s_swapImageViews.size(); ++i)
        {
            vk::ImageView attachment = s_swapImageViews[i];
            vk::FramebufferCreateInfo fbci;
            fbci.renderPass = s_overlayRenderPass;
            fbci.attachmentCount = 1;
            fbci.pAttachments = &attachment;
            fbci.width = s_swapExtent.width;
            fbci.height = s_swapExtent.height;
            fbci.layers = 1;
            s_overlayFramebuffers[i] = s_device.createFramebuffer(fbci);
        }
    }
    catch (const vk::SystemError& e)
    {
        VK_LOG_ERROR("Overlay render target creation failed: %s", e.what());
        return false;
    }

    return true;
}

bool FindMemoryType(uint32_t typeBits, vk::MemoryPropertyFlags properties, uint32_t& typeIndex)
{
    vk::PhysicalDeviceMemoryProperties memoryProperties = s_gpu.getMemoryProperties();
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
    {
        if ((typeBits & (1u << i)) &&
            (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties)
        {
            typeIndex = i;
            return true;
        }
    }
    return false;
}

void DestroyOverlayTextureResource(OverlayTextureResource& texture, bool removeDescriptor)
{
    if (removeDescriptor && s_overlayReady && texture.descriptor != VK_NULL_HANDLE)
        ImGui_ImplVulkan_RemoveTexture(texture.descriptor);
    texture.descriptor = VK_NULL_HANDLE;

    if (texture.sampler) s_device.destroySampler(texture.sampler);
    if (texture.view) s_device.destroyImageView(texture.view);
    if (texture.image) s_device.destroyImage(texture.image);
    if (texture.memory) s_device.freeMemory(texture.memory);
    texture = {};
}

void DestroyOverlayTextureResources()
{
    if (!s_device)
    {
        s_overlayTextures.clear();
        return;
    }

    try { s_device.waitIdle(); } catch (...) {}
    for (auto& upload : s_pendingOverlayUploads)
    {
        if (upload.stagingBuffer) s_device.destroyBuffer(upload.stagingBuffer);
        if (upload.stagingMemory) s_device.freeMemory(upload.stagingMemory);
    }
    s_pendingOverlayUploads.clear();
    for (auto& texture : s_overlayTextures)
        DestroyOverlayTextureResource(texture, true);
    s_overlayTextures.clear();
    for (auto& texture : s_retiredOverlayTextures)
        DestroyOverlayTextureResource(texture, true);
    s_retiredOverlayTextures.clear();
}

void CollectRetiredOverlayTextures()
{
    if (s_retiredOverlayTextures.empty() || !s_device) return;
    std::vector<vk::Fence> fences;
    fences.reserve(s_frames.size());
    for (const auto& frame : s_frames)
        if (frame.inflightFence) fences.push_back(frame.inflightFence);
    if (!fences.empty()) (void)s_device.waitForFences(fences, VK_TRUE, UINT64_MAX);
    for (auto& texture : s_retiredOverlayTextures)
        DestroyOverlayTextureResource(texture, true);
    VK_LOG_INFO("Released %zu retired overlay texture(s)", s_retiredOverlayTextures.size());
    s_retiredOverlayTextures.clear();
}

void RecordPendingOverlayUploads(PerFrame& frame)
{
    // Never submit an ad-hoc command buffer for a menu texture.  Flycast owns
    // work on this queue too; recording into the frontend frame keeps image
    // ownership and queue ordering identical to the regular composite path.
    for (PendingOverlayUpload& upload : s_pendingOverlayUploads)
    {
        if (!upload.image || !upload.stagingBuffer || upload.width == 0 || upload.height == 0)
            continue;

        TransitionLayout(frame.cmd, upload.image,
                         vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                         {}, vk::AccessFlagBits::eTransferWrite,
                         vk::PipelineStageFlagBits::eTopOfPipe,
                         vk::PipelineStageFlagBits::eTransfer);
        vk::BufferImageCopy region;
        region.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = vk::Extent3D(upload.width, upload.height, 1);
        frame.cmd.copyBufferToImage(upload.stagingBuffer, upload.image,
                                    vk::ImageLayout::eTransferDstOptimal, region);
        TransitionLayout(frame.cmd, upload.image,
                         vk::ImageLayout::eTransferDstOptimal,
                         vk::ImageLayout::eShaderReadOnlyOptimal,
                         vk::AccessFlagBits::eTransferWrite,
                         vk::AccessFlagBits::eShaderRead,
                         vk::PipelineStageFlagBits::eTransfer,
                         vk::PipelineStageFlagBits::eFragmentShader);
        frame.deferredUploadBuffers.push_back(upload.stagingBuffer);
        frame.deferredUploadMemories.push_back(upload.stagingMemory);
    }
    if (!s_pendingOverlayUploads.empty())
        VK_LOG_INFO("Recorded %zu pending overlay texture upload(s)", s_pendingOverlayUploads.size());
    s_pendingOverlayUploads.clear();
}

struct SlangVertex { float position[4]; float uv[2]; };

uint32_t SlangPushConstantSize(const GBAStationSlang::Pass& pass)
{
    uint32_t size = 0;
    for (const auto& member : pass.pushConstants)
        size = std::max(size, member.offset + member.size);
    return size;
}

uint32_t SlangUniformSize(const GBAStationSlang::Pass& pass)
{
    uint32_t size = 0;
    for (const auto& member : pass.uniformMembers)
        size = std::max(size, member.offset + member.size);
    return std::max<uint32_t>(size, 64);
}

uint32_t ResolveSlangDimension(GBAStationSlang::ScaleType type, float scale,
                               uint32_t source, uint32_t viewport)
{
    float value = type == GBAStationSlang::ScaleType::Absolute ? scale :
                  (type == GBAStationSlang::ScaleType::Viewport ? viewport : source) * scale;
    return std::max(1u, static_cast<uint32_t>(std::lround(std::max(1.0f, value))));
}

bool CreateHostBuffer(vk::DeviceSize size, vk::BufferUsageFlags usage,
                      vk::Buffer& buffer, vk::DeviceMemory& memory)
{
    vk::BufferCreateInfo info;
    info.size = size;
    info.usage = usage;
    info.sharingMode = vk::SharingMode::eExclusive;
    buffer = s_device.createBuffer(info);
    const vk::MemoryRequirements requirements = s_device.getBufferMemoryRequirements(buffer);
    uint32_t typeIndex = 0;
    if (!FindMemoryType(requirements.memoryTypeBits,
                        vk::MemoryPropertyFlagBits::eHostVisible |
                        vk::MemoryPropertyFlagBits::eHostCoherent, typeIndex) &&
        !FindMemoryType(requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eHostVisible, typeIndex))
        return false;
    vk::MemoryAllocateInfo allocation;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = typeIndex;
    memory = s_device.allocateMemory(allocation);
    s_device.bindBufferMemory(buffer, memory, 0);
    return true;
}

bool CreateSlangTarget(SlangRuntime& runtime, SlangTarget& target, vk::Extent2D extent)
{
    target.extent = extent;
    vk::ImageCreateInfo imageInfo;
    imageInfo.imageType = vk::ImageType::e2D;
    imageInfo.format = vk::Format::eR8G8B8A8Unorm;
    imageInfo.extent = vk::Extent3D(extent.width, extent.height, 1);
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = vk::SampleCountFlagBits::e1;
    imageInfo.tiling = vk::ImageTiling::eOptimal;
    imageInfo.usage = vk::ImageUsageFlagBits::eColorAttachment |
                      vk::ImageUsageFlagBits::eSampled |
                      vk::ImageUsageFlagBits::eTransferSrc;
    imageInfo.sharingMode = vk::SharingMode::eExclusive;
    target.image = s_device.createImage(imageInfo);
    const vk::MemoryRequirements requirements = s_device.getImageMemoryRequirements(target.image);
    uint32_t typeIndex = 0;
    if (!FindMemoryType(requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal, typeIndex))
        return false;
    vk::MemoryAllocateInfo allocation;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = typeIndex;
    target.memory = s_device.allocateMemory(allocation);
    s_device.bindImageMemory(target.image, target.memory, 0);

    vk::ImageViewCreateInfo viewInfo;
    viewInfo.image = target.image;
    viewInfo.viewType = vk::ImageViewType::e2D;
    viewInfo.format = vk::Format::eR8G8B8A8Unorm;
    viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    target.view = s_device.createImageView(viewInfo);

    vk::FramebufferCreateInfo framebufferInfo;
    framebufferInfo.renderPass = runtime.renderPass;
    framebufferInfo.attachmentCount = 1;
    framebufferInfo.pAttachments = &target.view;
    framebufferInfo.width = extent.width;
    framebufferInfo.height = extent.height;
    framebufferInfo.layers = 1;
    target.framebuffer = s_device.createFramebuffer(framebufferInfo);
    return true;
}

void DestroySlangRuntime(SlangRuntime& runtime)
{
    if (!s_device) return;
    for (auto& pass : runtime.passes)
    {
        for (auto& target : pass.targets)
        {
            if (target.framebuffer) s_device.destroyFramebuffer(target.framebuffer);
            if (target.view) s_device.destroyImageView(target.view);
            if (target.image) s_device.destroyImage(target.image);
            if (target.memory) s_device.freeMemory(target.memory);
        }
        for (auto buffer : pass.uniformBuffers) if (buffer) s_device.destroyBuffer(buffer);
        for (auto memory : pass.uniformMemories) if (memory) s_device.freeMemory(memory);
        if (pass.pipeline) s_device.destroyPipeline(pass.pipeline);
        if (pass.pipelineLayout) s_device.destroyPipelineLayout(pass.pipelineLayout);
        if (pass.descriptorSetLayout) s_device.destroyDescriptorSetLayout(pass.descriptorSetLayout);
    }
    if (runtime.vertexBuffer) s_device.destroyBuffer(runtime.vertexBuffer);
    if (runtime.vertexMemory) s_device.freeMemory(runtime.vertexMemory);
    if (runtime.nearestSampler) s_device.destroySampler(runtime.nearestSampler);
    if (runtime.linearSampler) s_device.destroySampler(runtime.linearSampler);
    if (runtime.descriptorPool) s_device.destroyDescriptorPool(runtime.descriptorPool);
    if (runtime.renderPass) s_device.destroyRenderPass(runtime.renderPass);
    runtime = {};
}

bool BuildSlangRuntime(SlangRuntime& runtime, const GBAStationSlang::Preset& preset, uint64_t presetVersion,
                       vk::Extent2D sourceExtent, vk::Extent2D viewportExtent)
{
    runtime.preset = &preset;
    runtime.presetVersion = presetVersion;
    runtime.sourceExtent = sourceExtent;
    runtime.viewportExtent = viewportExtent;
    const uint32_t frameCount = static_cast<uint32_t>(s_frames.size());
    if (frameCount == 0 || preset.passes.empty()) return false;
    VK_LOG_INFO("Slang preset path=%s passes=%zu runtimeParameters=%zu uiParameters=%zu",
                preset.path.c_str(), preset.passes.size(), preset.runtimeParameters.size(),
                preset.parameters.size());
    for (const std::string& warning : preset.warnings)
        VK_LOG_WARN("Slang preset warning: %s", warning.c_str());

    vk::AttachmentDescription attachment;
    attachment.format = vk::Format::eR8G8B8A8Unorm;
    attachment.samples = vk::SampleCountFlagBits::e1;
    attachment.loadOp = vk::AttachmentLoadOp::eDontCare;
    attachment.storeOp = vk::AttachmentStoreOp::eStore;
    attachment.stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
    attachment.stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
    attachment.initialLayout = vk::ImageLayout::eColorAttachmentOptimal;
    attachment.finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    vk::AttachmentReference colorReference(0, vk::ImageLayout::eColorAttachmentOptimal);
    vk::SubpassDescription subpass;
    subpass.pipelineBindPoint = vk::PipelineBindPoint::eGraphics;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorReference;
    const std::array<vk::SubpassDependency, 2> dependencies = {{
        {VK_SUBPASS_EXTERNAL, 0, vk::PipelineStageFlagBits::eFragmentShader,
         vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::AccessFlagBits::eShaderRead,
         vk::AccessFlagBits::eColorAttachmentWrite},
        {0, VK_SUBPASS_EXTERNAL, vk::PipelineStageFlagBits::eColorAttachmentOutput,
         vk::PipelineStageFlagBits::eFragmentShader, vk::AccessFlagBits::eColorAttachmentWrite,
         vk::AccessFlagBits::eShaderRead}}};
    vk::RenderPassCreateInfo renderPassInfo;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &attachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
    renderPassInfo.pDependencies = dependencies.data();
    runtime.renderPass = s_device.createRenderPass(renderPassInfo);

    uint32_t samplerCount = 0;
    for (const auto& pass : preset.passes) samplerCount += std::max<size_t>(1, pass.samplers.size());
    std::array<vk::DescriptorPoolSize, 2> poolSizes = {{
        {vk::DescriptorType::eUniformBuffer, static_cast<uint32_t>(preset.passes.size()) * frameCount},
        {vk::DescriptorType::eCombinedImageSampler, samplerCount * frameCount}}};
    vk::DescriptorPoolCreateInfo poolInfo;
    poolInfo.maxSets = static_cast<uint32_t>(preset.passes.size()) * frameCount;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    runtime.descriptorPool = s_device.createDescriptorPool(poolInfo);

    vk::SamplerCreateInfo samplerInfo;
    samplerInfo.magFilter = vk::Filter::eNearest;
    samplerInfo.minFilter = vk::Filter::eNearest;
    samplerInfo.mipmapMode = vk::SamplerMipmapMode::eNearest;
    samplerInfo.addressModeU = vk::SamplerAddressMode::eClampToEdge;
    samplerInfo.addressModeV = vk::SamplerAddressMode::eClampToEdge;
    samplerInfo.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    samplerInfo.maxLod = 0.0f;
    runtime.nearestSampler = s_device.createSampler(samplerInfo);
    samplerInfo.magFilter = vk::Filter::eLinear;
    samplerInfo.minFilter = vk::Filter::eLinear;
    samplerInfo.mipmapMode = vk::SamplerMipmapMode::eLinear;
    runtime.linearSampler = s_device.createSampler(samplerInfo);

    const std::array<SlangVertex, 4> vertices = {{{{-1.f, -1.f, 0.f, 1.f}, {0.f, 0.f}},
                                                    {{ 1.f, -1.f, 0.f, 1.f}, {1.f, 0.f}},
                                                    {{-1.f,  1.f, 0.f, 1.f}, {0.f, 1.f}},
                                                    {{ 1.f,  1.f, 0.f, 1.f}, {1.f, 1.f}}}};
    if (!CreateHostBuffer(sizeof(vertices), vk::BufferUsageFlagBits::eVertexBuffer,
                          runtime.vertexBuffer, runtime.vertexMemory))
        return false;
    void* vertexData = s_device.mapMemory(runtime.vertexMemory, 0, sizeof(vertices));
    std::memcpy(vertexData, vertices.data(), sizeof(vertices));
    s_device.flushMappedMemoryRanges(vk::MappedMemoryRange(runtime.vertexMemory, 0, sizeof(vertices)));
    s_device.unmapMemory(runtime.vertexMemory);

    vk::Extent2D inputExtent = sourceExtent;
    for (size_t passIndex = 0; passIndex < preset.passes.size(); ++passIndex)
    {
        const auto& definition = preset.passes[passIndex];
        SlangPassRuntime pass;
        pass.definition = &definition;
        pass.targetExtent = vk::Extent2D(
            ResolveSlangDimension(definition.scaleX, definition.scaleXValue, inputExtent.width, viewportExtent.width),
            ResolveSlangDimension(definition.scaleY, definition.scaleYValue, inputExtent.height, viewportExtent.height));
        pass.pushConstantSize = SlangPushConstantSize(definition);
        pass.uniformSize = SlangUniformSize(definition);
        if (pass.pushConstantSize > s_gpu.getProperties().limits.maxPushConstantsSize)
            throw std::runtime_error("Slang preset push-constant block exceeds device limit");

        std::map<uint32_t, vk::DescriptorSetLayoutBinding> bindings;
        bindings.emplace(0, vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eUniformBuffer, 1,
                                                             vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment));
        for (const auto& sampler : definition.samplers)
            bindings.emplace(sampler.binding, vk::DescriptorSetLayoutBinding(sampler.binding,
                vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment));
        std::vector<vk::DescriptorSetLayoutBinding> bindingList;
        for (const auto& binding : bindings) bindingList.push_back(binding.second);
        vk::DescriptorSetLayoutCreateInfo layoutInfo;
        layoutInfo.bindingCount = static_cast<uint32_t>(bindingList.size());
        layoutInfo.pBindings = bindingList.data();
        pass.descriptorSetLayout = s_device.createDescriptorSetLayout(layoutInfo);

        vk::PipelineLayoutCreateInfo pipelineLayoutInfo;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &pass.descriptorSetLayout;
        vk::PushConstantRange pushRange;
        if (pass.pushConstantSize > 0)
        {
            pushRange.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
            pushRange.offset = 0;
            pushRange.size = pass.pushConstantSize;
            pipelineLayoutInfo.pushConstantRangeCount = 1;
            pipelineLayoutInfo.pPushConstantRanges = &pushRange;
        }
        pass.pipelineLayout = s_device.createPipelineLayout(pipelineLayoutInfo);

        vk::ShaderModuleCreateInfo vertexModuleInfo({}, definition.vertexSpirv.size() * sizeof(uint32_t), definition.vertexSpirv.data());
        vk::ShaderModuleCreateInfo fragmentModuleInfo({}, definition.fragmentSpirv.size() * sizeof(uint32_t), definition.fragmentSpirv.data());
        vk::ShaderModule vertexModule = s_device.createShaderModule(vertexModuleInfo);
        vk::ShaderModule fragmentModule = s_device.createShaderModule(fragmentModuleInfo);
        const std::array<vk::PipelineShaderStageCreateInfo, 2> stages = {{
            {vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eVertex, vertexModule, "main"},
            {vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eFragment, fragmentModule, "main"}}};
        vk::VertexInputBindingDescription vertexBinding(0, sizeof(SlangVertex), vk::VertexInputRate::eVertex);
        const std::array<vk::VertexInputAttributeDescription, 2> attributes = {{
            {0, 0, vk::Format::eR32G32B32A32Sfloat, 0}, {1, 0, vk::Format::eR32G32Sfloat, 16}}};
        vk::PipelineVertexInputStateCreateInfo vertexInput;
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &vertexBinding;
        vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
        vertexInput.pVertexAttributeDescriptions = attributes.data();
        vk::PipelineInputAssemblyStateCreateInfo inputAssembly({}, vk::PrimitiveTopology::eTriangleStrip, VK_FALSE);
        vk::PipelineViewportStateCreateInfo viewportState({}, 1, nullptr, 1, nullptr);
        vk::PipelineRasterizationStateCreateInfo rasterization({}, VK_FALSE, VK_FALSE, vk::PolygonMode::eFill,
            vk::CullModeFlagBits::eNone, vk::FrontFace::eCounterClockwise, VK_FALSE, 0, 0, 0, 1.0f);
        vk::PipelineMultisampleStateCreateInfo multisample({}, vk::SampleCountFlagBits::e1);
        vk::PipelineColorBlendAttachmentState blendAttachment(VK_FALSE);
        blendAttachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                         vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        vk::PipelineColorBlendStateCreateInfo colorBlend({}, VK_FALSE, vk::LogicOp::eCopy, 1, &blendAttachment);
        const std::array<vk::DynamicState, 2> dynamicStates = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
        vk::PipelineDynamicStateCreateInfo dynamic({}, static_cast<uint32_t>(dynamicStates.size()), dynamicStates.data());
        vk::GraphicsPipelineCreateInfo pipelineInfo;
        pipelineInfo.stageCount = static_cast<uint32_t>(stages.size());
        pipelineInfo.pStages = stages.data();
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterization;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pColorBlendState = &colorBlend;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = pass.pipelineLayout;
        pipelineInfo.renderPass = runtime.renderPass;
        pass.pipeline = s_device.createGraphicsPipeline({}, pipelineInfo).value;
        s_device.destroyShaderModule(vertexModule);
        s_device.destroyShaderModule(fragmentModule);

        std::vector<vk::DescriptorSetLayout> setLayouts(frameCount, pass.descriptorSetLayout);
        vk::DescriptorSetAllocateInfo allocateInfo(runtime.descriptorPool, frameCount, setLayouts.data());
        pass.descriptorSets = s_device.allocateDescriptorSets(allocateInfo);
        pass.uniformBuffers.resize(frameCount);
        pass.uniformMemories.resize(frameCount);
        pass.targets.resize(frameCount);
        for (uint32_t frame = 0; frame < frameCount; ++frame)
        {
            if (!CreateHostBuffer(pass.uniformSize, vk::BufferUsageFlagBits::eUniformBuffer,
                                  pass.uniformBuffers[frame], pass.uniformMemories[frame]) ||
                !CreateSlangTarget(runtime, pass.targets[frame], pass.targetExtent))
                return false;
        }
        inputExtent = pass.targetExtent;
        VK_LOG_INFO("Slang pass[%zu] target=%ux%u push=%u ubo=%u samplers=%zu uniforms=%zu alias=%s",
                    passIndex, pass.targetExtent.width, pass.targetExtent.height, pass.pushConstantSize,
                    pass.uniformSize, definition.samplers.size(), definition.uniformMembers.size(),
                    definition.alias.empty() ? "<final>" : definition.alias.c_str());
        for (const auto& sampler : definition.samplers)
            VK_LOG_INFO("  sampler binding=%u name=%s", sampler.binding, sampler.name.c_str());
        for (const auto& member : definition.uniformMembers)
            VK_LOG_INFO("  ubo member=%s offset=%u size=%u", member.name.c_str(), member.offset, member.size);
        runtime.passes.push_back(std::move(pass));
    }
    VK_LOG_INFO("Slang chain ready: passes=%zu source=%ux%u viewport=%ux%u", runtime.passes.size(),
                sourceExtent.width, sourceExtent.height, viewportExtent.width, viewportExtent.height);
    return true;
}

SlangRuntime* FindSlangRuntime(const GBAStationSlang::Preset& preset, uint64_t presetVersion,
                               vk::Extent2D sourceExtent, vk::Extent2D viewportExtent)
{
    for (auto& runtime : s_slangRuntimes)
        if (runtime.preset == &preset && runtime.presetVersion == presetVersion && runtime.sourceExtent == sourceExtent &&
            runtime.viewportExtent == viewportExtent)
        {
            s_slangKeepSourceExtent = sourceExtent;
            s_slangKeepViewportExtent = viewportExtent;
            return &runtime;
        }
    try
    {
        SlangRuntime runtime;
        if (!BuildSlangRuntime(runtime, preset, presetVersion, sourceExtent, viewportExtent))
        {
            DestroySlangRuntime(runtime);
            return nullptr;
        }
        s_slangRuntimes.push_back(std::move(runtime));
        s_slangKeepSourceExtent = sourceExtent;
        s_slangKeepViewportExtent = viewportExtent;
        s_slangCleanupRequested = true;
        return &s_slangRuntimes.back();
    }
    catch (const std::exception& error)
    {
        const std::string key = preset.path + ": " + error.what();
        if (key != s_slangLastFailure)
        {
            s_slangLastFailure = key;
            VK_LOG_ERROR("Slang chain build failed: %s", key.c_str());
        }
        return nullptr;
    }
}

void CollectRetiredSlangRuntimes()
{
    if (!s_slangCleanupRequested || !s_device) return;
    std::vector<vk::Fence> fences;
    fences.reserve(s_frames.size());
    for (const auto& frame : s_frames)
        if (frame.inflightFence) fences.push_back(frame.inflightFence);
    if (!fences.empty()) (void)s_device.waitForFences(fences, VK_TRUE, UINT64_MAX);

    std::vector<SlangRuntime> retained;
    retained.reserve(1);
    for (auto& runtime : s_slangRuntimes)
    {
        if (runtime.preset == s_activeSlangPreset && runtime.presetVersion == s_activeSlangPresetVersion &&
            runtime.sourceExtent == s_slangKeepSourceExtent &&
            runtime.viewportExtent == s_slangKeepViewportExtent)
            retained.push_back(std::move(runtime));
        else
            DestroySlangRuntime(runtime);
    }
    s_slangRuntimes = std::move(retained);
    s_slangCleanupRequested = false;
}

struct SlangInput
{
    vk::Image image;
    vk::ImageView view;
    vk::Extent2D extent{};
};

bool RenderSlangFrame(PerFrame& frame, const retro_vulkan_image& coreSource,
                      vk::Extent2D sourceExtent, SlangOutput& output)
{
    const GBAStationSlang::Preset* preset = s_activeSlangPreset;
    if (!preset || preset->passes.empty() || coreSource.image_view == VK_NULL_HANDLE)
        return false;
    SlangRuntime* runtime = FindSlangRuntime(*preset, s_activeSlangPresetVersion, sourceExtent, s_swapExtent);
    if (!runtime || s_currentFrame >= s_frames.size()) return false;

    const vk::Image coreImage(coreSource.create_info.image);
    const vk::ImageLayout coreLayout = static_cast<vk::ImageLayout>(coreSource.image_layout);
    // Flycast usually hands us an image that is already in SHADER_READ_ONLY
    // layout. Layout equality does not make the core's color writes visible to
    // this command buffer, though; retain this same-layout barrier so sampling
    // never sees an uninitialised (black) cache line.
    TransitionLayout(frame.cmd, coreImage, coreLayout, vk::ImageLayout::eShaderReadOnlyOptimal,
                     vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
                     vk::AccessFlagBits::eShaderRead, vk::PipelineStageFlagBits::eAllCommands,
                     vk::PipelineStageFlagBits::eFragmentShader);
    const SlangInput original{coreImage, vk::ImageView(coreSource.image_view), sourceExtent};
    SlangInput current = original;
    std::map<std::string, SlangInput> aliases;
    const bool diagnostic = runtime->diagnosticFrames < 3;
    if (diagnostic)
        VK_LOG_INFO("Slang frame source image=%p view=%p layout=%d extent=%ux%u passes=%zu",
                    static_cast<VkImage>(coreImage), coreSource.image_view, static_cast<int>(coreLayout),
                    sourceExtent.width, sourceExtent.height, runtime->passes.size());

    for (SlangPassRuntime& pass : runtime->passes)
    {
        const GBAStationSlang::Pass& definition = *pass.definition;
        SlangTarget& target = pass.targets[s_currentFrame];
        const vk::ImageLayout oldLayout = target.initialized ? vk::ImageLayout::eShaderReadOnlyOptimal :
                                                              vk::ImageLayout::eUndefined;
        TransitionLayout(frame.cmd, target.image, oldLayout, vk::ImageLayout::eColorAttachmentOptimal,
                         oldLayout == vk::ImageLayout::eUndefined ? vk::AccessFlags() : vk::AccessFlagBits::eShaderRead,
                         vk::AccessFlagBits::eColorAttachmentWrite,
                         oldLayout == vk::ImageLayout::eUndefined ? vk::PipelineStageFlagBits::eTopOfPipe : vk::PipelineStageFlagBits::eFragmentShader,
                         vk::PipelineStageFlagBits::eColorAttachmentOutput);

        std::vector<uint8_t> uniformDataBytes(pass.uniformSize, 0);
        auto writeUniformVec4 = [&uniformDataBytes, &definition](const std::string& name, vk::Extent2D extent) {
            const float values[4] = {static_cast<float>(extent.width), static_cast<float>(extent.height),
                                     extent.width ? 1.0f / extent.width : 0.0f,
                                     extent.height ? 1.0f / extent.height : 0.0f};
            for (const auto& member : definition.uniformMembers)
                if (member.name == name && member.size >= sizeof(values))
                    std::memcpy(uniformDataBytes.data() + member.offset, values, sizeof(values));
        };
        auto writeUniformMemberSize = [&uniformDataBytes](const GBAStationSlang::PushConstantMember& member,
                                                            vk::Extent2D extent) {
            if (member.size < sizeof(float) * 4) return;
            const float values[4] = {static_cast<float>(extent.width), static_cast<float>(extent.height),
                                     extent.width ? 1.0f / extent.width : 0.0f,
                                     extent.height ? 1.0f / extent.height : 0.0f};
            std::memcpy(uniformDataBytes.data() + member.offset, values, sizeof(values));
        };
        auto writeUniformUint = [&uniformDataBytes, &definition](const std::string& name, uint32_t value) {
            for (const auto& member : definition.uniformMembers)
                if (member.name == name && member.size >= sizeof(value))
                    std::memcpy(uniformDataBytes.data() + member.offset, &value, sizeof(value));
        };
        for (const auto& member : definition.uniformMembers)
            if (member.name == "MVP" && member.size >= 64)
            {
                const float identity[16] = {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f,
                                            0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f};
                std::memcpy(uniformDataBytes.data() + member.offset, identity, sizeof(identity));
            }
        writeUniformVec4("OutputSize", target.extent);
        writeUniformVec4("FinalViewportSize", s_swapExtent);
        writeUniformVec4("SourceSize", current.extent);
        writeUniformVec4("OriginalSize", original.extent);
        // RetroArch Slang exposes each sampled alias as <alias>Size.  The
        // reflection must populate these too: e.g. diffusion-h samples
        // ColorAdj while PP-reflex samples PhosphorLine. Leaving those blocks
        // at zero collapses texture coordinates and produces a black frame.
        for (const auto& member : definition.uniformMembers)
        {
            constexpr const char* suffix = "Size";
            if (member.name.size() <= 4 || member.name.compare(member.name.size() - 4, 4, suffix) != 0)
                continue;
            const std::string sourceName = member.name.substr(0, member.name.size() - 4);
            if (sourceName == "Source") writeUniformMemberSize(member, current.extent);
            else if (sourceName == "Original") writeUniformMemberSize(member, original.extent);
            else if (sourceName == "FinalViewport") writeUniformMemberSize(member, s_swapExtent);
            else if (sourceName != "Output")
            {
                const auto alias = aliases.find(sourceName);
                if (alias != aliases.end()) writeUniformMemberSize(member, alias->second.extent);
            }
        }
        writeUniformUint("FrameCount", runtime->frameCount);
        void* uniformData = s_device.mapMemory(pass.uniformMemories[s_currentFrame], 0, pass.uniformSize);
        std::memcpy(uniformData, uniformDataBytes.data(), uniformDataBytes.size());
        s_device.flushMappedMemoryRanges(vk::MappedMemoryRange(pass.uniformMemories[s_currentFrame], 0, pass.uniformSize));
        s_device.unmapMemory(pass.uniformMemories[s_currentFrame]);

        vk::DescriptorBufferInfo bufferInfo(pass.uniformBuffers[s_currentFrame], 0, pass.uniformSize);
        std::vector<vk::DescriptorImageInfo> imageInfos;
        imageInfos.reserve(definition.samplers.size());
        std::vector<vk::WriteDescriptorSet> writes;
        writes.reserve(definition.samplers.size() + 1);
        writes.emplace_back(pass.descriptorSets[s_currentFrame], 0, 0, 1,
                            vk::DescriptorType::eUniformBuffer, nullptr, &bufferInfo);
        for (const auto& sampler : definition.samplers)
        {
            SlangInput input = current;
            const char* sourceName = "previous-pass";
            if (sampler.name == "Original") input = original;
            if (sampler.name == "Original") sourceName = "original";
            else if (sampler.name != "Source")
            {
                const auto alias = aliases.find(sampler.name);
                if (alias != aliases.end()) { input = alias->second; sourceName = "alias"; }
                else sourceName = "missing-alias/fallback-previous";
            }
            if (diagnostic)
                VK_LOG_INFO("Slang bind pass=%s binding=%u sampler=%s source=%s image=%p extent=%ux%u",
                            definition.path.c_str(), sampler.binding, sampler.name.c_str(), sourceName,
                            static_cast<VkImage>(input.image), input.extent.width, input.extent.height);
            imageInfos.emplace_back(definition.linear ? runtime->linearSampler : runtime->nearestSampler,
                                    input.view, vk::ImageLayout::eShaderReadOnlyOptimal);
            writes.emplace_back(pass.descriptorSets[s_currentFrame], sampler.binding, 0, 1,
                                vk::DescriptorType::eCombinedImageSampler, &imageInfos.back());
        }
        s_device.updateDescriptorSets(writes, nullptr);

        std::vector<uint8_t> pushConstants(pass.pushConstantSize, 0);
        auto writeFloat = [&pushConstants, &definition](const std::string& name, float value) {
            for (const auto& member : definition.pushConstants)
                if (member.name == name && member.size >= sizeof(float))
                    std::memcpy(pushConstants.data() + member.offset, &value, sizeof(float));
        };
        auto writeVec4 = [&pushConstants, &definition](const std::string& name, vk::Extent2D extent) {
            const float values[4] = {static_cast<float>(extent.width), static_cast<float>(extent.height),
                                     extent.width ? 1.0f / extent.width : 0.0f,
                                     extent.height ? 1.0f / extent.height : 0.0f};
            for (const auto& member : definition.pushConstants)
                if (member.name == name && member.size >= sizeof(values))
                    std::memcpy(pushConstants.data() + member.offset, values, sizeof(values));
        };
        auto writePushMemberSize = [&pushConstants](const GBAStationSlang::PushConstantMember& member,
                                                     vk::Extent2D extent) {
            if (member.size < sizeof(float) * 4) return;
            const float values[4] = {static_cast<float>(extent.width), static_cast<float>(extent.height),
                                     extent.width ? 1.0f / extent.width : 0.0f,
                                     extent.height ? 1.0f / extent.height : 0.0f};
            std::memcpy(pushConstants.data() + member.offset, values, sizeof(values));
        };
        auto writePushUint = [&pushConstants, &definition](const std::string& name, uint32_t value) {
            for (const auto& member : definition.pushConstants)
                if (member.name == name && member.size >= sizeof(value))
                    std::memcpy(pushConstants.data() + member.offset, &value, sizeof(value));
        };
        writeVec4("SourceSize", current.extent);
        writeVec4("OriginalSize", original.extent);
        writeVec4("OutputSize", target.extent);
        writeVec4("FinalViewportSize", s_swapExtent);
        for (const auto& member : definition.pushConstants)
        {
            constexpr const char* suffix = "Size";
            if (member.name.size() <= 4 || member.name.compare(member.name.size() - 4, 4, suffix) != 0)
                continue;
            const std::string sourceName = member.name.substr(0, member.name.size() - 4);
            if (sourceName == "Source") writePushMemberSize(member, current.extent);
            else if (sourceName == "Original") writePushMemberSize(member, original.extent);
            else if (sourceName == "FinalViewport") writePushMemberSize(member, s_swapExtent);
            else if (sourceName != "Output")
            {
                const auto alias = aliases.find(sourceName);
                if (alias != aliases.end()) writePushMemberSize(member, alias->second.extent);
            }
        }
        writePushUint("FrameCount", runtime->frameCount);
        // runtimeParameters contains all pragma defaults; parameters only
        // contains the controls intentionally exposed by the preset.  Overlay
        // edits take precedence without mutating the parsed runtime defaults.
        for (const auto& parameter : preset->runtimeParameters)
        {
            float value = parameter.value;
            const auto uiParameter = std::find_if(preset->parameters.begin(), preset->parameters.end(),
                [&parameter](const GBAStationSlang::Parameter& candidate) {
                    return candidate.runtimeId == parameter.runtimeId;
                });
            if (uiParameter != preset->parameters.end()) value = uiParameter->value;
            writeFloat(parameter.runtimeId, value);
        }
        for (const auto& parameter : preset->parameters)
        {
            writeFloat(parameter.id, parameter.value);
            if (parameter.runtimeId != parameter.id) writeFloat(parameter.runtimeId, parameter.value);
        }

        vk::RenderPassBeginInfo renderInfo;
        renderInfo.renderPass = runtime->renderPass;
        renderInfo.framebuffer = target.framebuffer;
        renderInfo.renderArea.extent = target.extent;
        frame.cmd.beginRenderPass(renderInfo, vk::SubpassContents::eInline);
        frame.cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pass.pipeline);
        const vk::Viewport viewport(0.0f, 0.0f, static_cast<float>(target.extent.width),
                                    static_cast<float>(target.extent.height), 0.0f, 1.0f);
        frame.cmd.setViewport(0, viewport);
        frame.cmd.setScissor(0, vk::Rect2D(vk::Offset2D(0, 0), target.extent));
        frame.cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pass.pipelineLayout, 0,
                                     pass.descriptorSets[s_currentFrame], nullptr);
        const vk::DeviceSize offset = 0;
        frame.cmd.bindVertexBuffers(0, runtime->vertexBuffer, offset);
        if (!pushConstants.empty())
            frame.cmd.pushConstants(pass.pipelineLayout, vk::ShaderStageFlagBits::eVertex |
                                    vk::ShaderStageFlagBits::eFragment, 0,
                                    static_cast<uint32_t>(pushConstants.size()), pushConstants.data());
        frame.cmd.draw(4, 1, 0, 0);
        frame.cmd.endRenderPass();
        vk::MemoryBarrier barrier(vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eShaderRead);
        frame.cmd.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                  vk::PipelineStageFlagBits::eFragmentShader, {}, barrier, {}, {});
        target.initialized = true;
        current = {target.image, target.view, target.extent};
        if (!definition.alias.empty()) aliases[definition.alias] = current;
        if (diagnostic)
            VK_LOG_INFO("Slang frame pass output image=%p extent=%ux%u alias=%s",
                        static_cast<VkImage>(target.image), target.extent.width, target.extent.height,
                        definition.alias.empty() ? "<final>" : definition.alias.c_str());
    }
    if (coreLayout != vk::ImageLayout::eShaderReadOnlyOptimal)
        TransitionLayout(frame.cmd, coreImage, vk::ImageLayout::eShaderReadOnlyOptimal, coreLayout,
                         vk::AccessFlagBits::eShaderRead,
                         vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
                         vk::PipelineStageFlagBits::eFragmentShader, vk::PipelineStageFlagBits::eAllCommands);
    output.image = current.image;
    output.extent = current.extent;
    output.valid = true;
    ++runtime->frameCount;
    if (diagnostic) ++runtime->diagnosticFrames;
    return true;
}

void DestroyOverlayRenderTargets()
{
    for (auto& fb : s_overlayFramebuffers)
        if (fb) s_device.destroyFramebuffer(fb);
    s_overlayFramebuffers.clear();

    if (s_overlayRenderPass) s_device.destroyRenderPass(s_overlayRenderPass);
    s_overlayRenderPass = nullptr;
}

void ShutdownOverlayRendererInternal()
{
    DestroyOverlayTextureResources();

    if (s_overlayReady)
    {
        ImGui_ImplVulkan_Shutdown();
        s_overlayReady = false;
    }

    if (s_overlayDescriptorPool)
    {
        s_device.destroyDescriptorPool(s_overlayDescriptorPool);
        s_overlayDescriptorPool = nullptr;
    }

    s_overlayDrawData = nullptr;
}

// --- libretro Vulkan interface callbacks ---------------------------------

void RETRO_CALLCONV cb_set_image(void* /*handle*/,
                                 const retro_vulkan_image* image,
                                 uint32_t /*num_semaphores*/,
                                 const VkSemaphore* /*semaphores*/,
                                 uint32_t /*src_queue_family*/)
{
    if (!image)
        return;
    PerFrame& f = s_frames[s_currentFrame];
    f.image = *image;
    f.imageValid = true;
    s_lastImage = *image;
    s_lastImageValid = image->create_info.image != VK_NULL_HANDLE;
    s_lastImageFrame = s_currentFrame;
}

uint32_t RETRO_CALLCONV cb_get_sync_index(void* /*handle*/)
{
    return s_currentFrame;
}

uint32_t RETRO_CALLCONV cb_get_sync_index_mask(void* /*handle*/)
{
    // Bitmask over all currently used sync indices.
    return s_frames.empty() ? 1u : ((1u << s_frames.size()) - 1u);
}

void RETRO_CALLCONV cb_set_command_buffers(void* /*handle*/,
                                           uint32_t num_cmd,
                                           const VkCommandBuffer* cmd)
{
    PerFrame& f = s_frames[s_currentFrame];
    f.coreCommandBuffers.assign(cmd, cmd + num_cmd);
}

void RETRO_CALLCONV cb_wait_sync_index(void* /*handle*/)
{
    if (s_frames.empty() || !s_device)
        return;
    PerFrame& f = s_frames[s_currentFrame];
    if (f.inflightFence)
        (void)s_device.waitForFences(f.inflightFence, VK_TRUE, UINT64_MAX);
}

void RETRO_CALLCONV cb_lock_queue(void* /*handle*/)
{
    s_queueMutex.lock();
}

void RETRO_CALLCONV cb_unlock_queue(void* /*handle*/)
{
    s_queueMutex.unlock();
}

void RETRO_CALLCONV cb_set_signal_semaphore(void* /*handle*/, VkSemaphore semaphore)
{
    PerFrame& f = s_frames[s_currentFrame];
    f.signalSemaphore = semaphore;
}

// --- Bringup -------------------------------------------------------------

bool CreateInstanceInternal()
{
    VK_LOG_INFO("CreateInstanceInternal begin");
    PFN_vkGetInstanceProcAddr getInstProcAddr = GetInstanceProcAddrFunc();
    if (!getInstProcAddr)
    {
        VK_LOG_ERROR("vk_icdGetInstanceProcAddr unavailable");
        return false;
    }

#if VULKAN_HPP_DISPATCH_LOADER_DYNAMIC == 1
    VK_LOG_INFO("dispatcher init global begin");
    VULKAN_HPP_DEFAULT_DISPATCHER.init(getInstProcAddr);
    VK_LOG_INFO("dispatcher init global ok");
#endif

    vk::ApplicationInfo appInfo("GBAStation-flycast", 1, "Flycast", 1, VK_API_VERSION_1_1);

    std::vector<const char*> extensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
#if defined(__SWITCH__) && defined(VK_NN_VI_SURFACE_EXTENSION_NAME)
        VK_NN_VI_SURFACE_EXTENSION_NAME,
#endif
    };

    vk::InstanceCreateInfo createInfo({}, &appInfo, nullptr, extensions);

    try
    {
        VK_LOG_INFO("vk::createInstance begin");
        s_instance = vk::createInstance(createInfo);
        g_GBAStationIcdInstance = static_cast<VkInstance>(s_instance);
    }
    catch (const vk::SystemError& e)
    {
        VK_LOG_ERROR("vkCreateInstance failed: %s", e.what());
        return false;
    }

#if VULKAN_HPP_DISPATCH_LOADER_DYNAMIC == 1
    VK_LOG_INFO("dispatcher init instance begin");
    VULKAN_HPP_DEFAULT_DISPATCHER.init(s_instance);
    VK_LOG_INFO("dispatcher init instance ok");
#endif

    VK_LOG_INFO("VkInstance created (%zu extensions)", extensions.size());
    return true;
}

bool CreateSurfaceInternal()
{
#if defined(__SWITCH__)
    VK_LOG_INFO("CreateSurfaceInternal begin");
    // VK_NN_vi_surface — the Switch surface extension.
    // Homebrew inherits the application's previous VI dimensions.  When a
    // chainloaded title leaves it at 640x360, the compositor scales both the
    // game and ImGui overlay to 720p, making the lower-left quadrant appear
    // as a doubled fullscreen image.  Flycast always owns a 720p landscape
    // output surface.
    if (!s_nwindow)
        s_nwindow = nwindowGetDefault();
    nwindowSetDimensions(s_nwindow, 1280, 720);
    vk::ViSurfaceCreateInfoNN createInfo;
    createInfo.window = s_nwindow;

    try
    {
        s_surface = s_instance.createViSurfaceNN(createInfo);
    }
    catch (const vk::SystemError& e)
    {
        VK_LOG_ERROR("vkCreateViSurfaceNN failed: %s", e.what());
        return false;
    }
    VK_LOG_INFO("VkSurfaceKHR created via VI_NN");
    return true;
#else
    VK_LOG_ERROR("Surface creation not supported on this platform");
    return false;
#endif
}

bool CreateDeviceInternal()
{
    VK_LOG_INFO("CreateDeviceInternal begin negIface=%p create_device=%p", s_negIface,
                s_negIface ? reinterpret_cast<const void*>(s_negIface->create_device) : nullptr);
    PFN_vkGetInstanceProcAddr getInstProcAddr = GetInstanceProcAddrFunc();

    // Path A: core registered a negotiation interface — let it create the device.
    if (s_negIface && s_negIface->create_device)
    {
        retro_vulkan_context ctx{};
        VK_LOG_INFO("core create_device begin");
        const bool ok = s_negIface->create_device(&ctx,
                                                  static_cast<VkInstance>(s_instance),
                                                  VK_NULL_HANDLE,
                                                  static_cast<VkSurfaceKHR>(s_surface),
                                                  getInstProcAddr,
                                                  nullptr, 0,
                                                  nullptr, 0,
                                                  nullptr);
        if (!ok)
        {
            VK_LOG_ERROR("Core create_device returned false");
            return false;
        }

        s_gpu = vk::PhysicalDevice(ctx.gpu);
        s_device = vk::Device(ctx.device);
        s_queue = vk::Queue(ctx.queue);
        // The negotiation contract permits a separate presentation queue.
        // Keep it distinct from the graphics queue rather than assuming the
        // Switch implementation always exposes a combined family.
        s_presentQueue = vk::Queue(ctx.presentation_queue ? ctx.presentation_queue : ctx.queue);
        s_queueFamilyIndex = ctx.queue_family_index;

#if VULKAN_HPP_DISPATCH_LOADER_DYNAMIC == 1
        VK_LOG_INFO("dispatcher init device begin");
        VULKAN_HPP_DEFAULT_DISPATCHER.init(s_device);
        VK_LOG_INFO("dispatcher init device ok");
#endif
        VK_LOG_INFO("Device created via core negotiation (qfi=%u gfx=%p present=%p)",
                    s_queueFamilyIndex, static_cast<VkQueue>(s_queue),
                    static_cast<VkQueue>(s_presentQueue));
        return true;
    }

    // Path B: pick a GPU + create a device ourselves. Used if the core didn't
    // register a negotiation interface (shouldn't happen for flycast, but
    // libretro spec allows it).
    auto gpus = s_instance.enumeratePhysicalDevices();
    VK_LOG_INFO("fallback enumeratePhysicalDevices count=%zu", gpus.size());
    if (gpus.empty())
    {
        VK_LOG_ERROR("No Vulkan physical devices");
        return false;
    }
    s_gpu = gpus[0];
    for (const auto& g : gpus)
    {
        auto props = g.getProperties();
        if (props.deviceType == vk::PhysicalDeviceType::eDiscreteGpu)
        {
            s_gpu = g;
            break;
        }
    }

    auto qprops = s_gpu.getQueueFamilyProperties();
    s_queueFamilyIndex = static_cast<uint32_t>(qprops.size());
    for (uint32_t i = 0; i < qprops.size(); ++i)
    {
        const bool gfx = (qprops[i].queueFlags & vk::QueueFlagBits::eGraphics) ==
                         vk::QueueFlagBits::eGraphics;
        const bool present = s_gpu.getSurfaceSupportKHR(i, s_surface);
        if (gfx && present)
        {
            s_queueFamilyIndex = i;
            break;
        }
    }
    if (s_queueFamilyIndex >= qprops.size())
    {
        VK_LOG_ERROR("No graphics+present queue family");
        return false;
    }

    float prio = 1.0f;
    vk::DeviceQueueCreateInfo qci({}, s_queueFamilyIndex, 1, &prio);
    std::vector<const char*> deviceExt = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    vk::DeviceCreateInfo dci({}, qci, nullptr, deviceExt);

    try
    {
        s_device = s_gpu.createDevice(dci);
    }
    catch (const vk::SystemError& e)
    {
        VK_LOG_ERROR("vkCreateDevice failed: %s", e.what());
        return false;
    }

#if VULKAN_HPP_DISPATCH_LOADER_DYNAMIC == 1
    VULKAN_HPP_DEFAULT_DISPATCHER.init(s_device);
#endif
    s_queue = s_device.getQueue(s_queueFamilyIndex, 0);
    s_presentQueue = s_queue;
    VK_LOG_INFO("Device created via fallback path (qfi=%u)", s_queueFamilyIndex);
    return true;
}

bool CreateSwapchainInternal()
{
    VK_LOG_INFO("CreateSwapchainInternal begin");
    auto caps = s_gpu.getSurfaceCapabilitiesKHR(s_surface);
    auto formats = s_gpu.getSurfaceFormatsKHR(s_surface);
    auto modes = s_gpu.getSurfacePresentModesKHR(s_surface);
    VK_LOG_INFO("surface caps extent=%ux%u formats=%zu modes=%zu", caps.currentExtent.width,
                caps.currentExtent.height, formats.size(), modes.size());

    if (formats.empty())
    {
        VK_LOG_ERROR("No surface formats");
        return false;
    }

    vk::SurfaceFormatKHR fmt = formats[0];
    for (const auto& f : formats)
    {
        if (f.format == vk::Format::eB8G8R8A8Unorm || f.format == vk::Format::eR8G8B8A8Unorm)
        {
            fmt = f;
            break;
        }
    }
    s_swapFormat = fmt.format;

    s_swapExtent = caps.currentExtent;
    if (s_swapExtent.width == UINT32_MAX)
    {
        // Switch nwindow always reports a real extent; this is a safety net.
        s_swapExtent.width = std::clamp<uint32_t>(1280, caps.minImageExtent.width, caps.maxImageExtent.width);
        s_swapExtent.height = std::clamp<uint32_t>(720, caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    vk::PresentModeKHR mode = vk::PresentModeKHR::eFifo;  // vsync, always supported
    (void)modes;

    uint32_t imageCount = std::max<uint32_t>(3, caps.minImageCount);
    if (caps.maxImageCount != 0)
        imageCount = std::min(imageCount, caps.maxImageCount);

    vk::SwapchainCreateInfoKHR sci;
    sci.surface = s_surface;
    sci.minImageCount = imageCount;
    sci.imageFormat = fmt.format;
    sci.imageColorSpace = fmt.colorSpace;
    sci.imageExtent = s_swapExtent;
    sci.imageArrayLayers = 1;
    // TRANSFER_DST so we can blit core's image onto it.
    sci.imageUsage = vk::ImageUsageFlagBits::eColorAttachment |
                     vk::ImageUsageFlagBits::eTransferDst;
    sci.imageSharingMode = vk::SharingMode::eExclusive;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
    sci.presentMode = mode;
    sci.clipped = VK_TRUE;

    try
    {
        s_swapchain = s_device.createSwapchainKHR(sci);
    }
    catch (const vk::SystemError& e)
    {
        VK_LOG_ERROR("vkCreateSwapchainKHR failed: %s", e.what());
        return false;
    }

    s_swapImages = s_device.getSwapchainImagesKHR(s_swapchain);
    s_swapImageViews.resize(s_swapImages.size());
    for (size_t i = 0; i < s_swapImages.size(); ++i)
    {
        vk::ImageViewCreateInfo vci;
        vci.image = s_swapImages[i];
        vci.viewType = vk::ImageViewType::e2D;
        vci.format = s_swapFormat;
        vci.components = vk::ComponentMapping();
        vci.subresourceRange = vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
        s_swapImageViews[i] = s_device.createImageView(vci);
    }

    VK_LOG_INFO("Swapchain: %ux%u format=%d images=%zu",
                s_swapExtent.width, s_swapExtent.height,
                static_cast<int>(s_swapFormat), s_swapImages.size());
    return true;
}

bool CreateFrameResources()
{
    vk::CommandPoolCreateInfo cpci(vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
                                   s_queueFamilyIndex);
    s_commandPool = s_device.createCommandPool(cpci);

    const uint32_t frameCount = static_cast<uint32_t>(s_swapImages.size());
    s_frames.resize(frameCount);

    vk::CommandBufferAllocateInfo cbai(s_commandPool, vk::CommandBufferLevel::ePrimary, frameCount);
    auto cmds = s_device.allocateCommandBuffers(cbai);

    for (uint32_t i = 0; i < frameCount; ++i)
    {
        s_frames[i].cmd = cmds[i];
        s_frames[i].inflightFence = s_device.createFence(
            vk::FenceCreateInfo(vk::FenceCreateFlagBits::eSignaled));
        s_frames[i].acquireSemaphore = s_device.createSemaphore({});
        s_frames[i].renderSemaphore = s_device.createSemaphore({});
    }
    return true;
}

void PopulateHwInterface()
{
    s_hwIface.interface_type = RETRO_HW_RENDER_INTERFACE_VULKAN;
    s_hwIface.interface_version = RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION;
    s_hwIface.handle = nullptr;
    s_hwIface.instance = static_cast<VkInstance>(s_instance);
    s_hwIface.gpu = static_cast<VkPhysicalDevice>(s_gpu);
    s_hwIface.device = static_cast<VkDevice>(s_device);
    s_hwIface.queue = static_cast<VkQueue>(s_queue);
    s_hwIface.queue_index = s_queueFamilyIndex;

    PFN_vkGetInstanceProcAddr getInstProcAddr = GetInstanceProcAddrFunc();
    s_hwIface.get_instance_proc_addr = getInstProcAddr;
    s_hwIface.get_device_proc_addr =
        getInstProcAddr
            ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(
                  getInstProcAddr(static_cast<VkInstance>(s_instance), "vkGetDeviceProcAddr"))
            : nullptr;

    s_hwIface.set_image = &cb_set_image;
    s_hwIface.get_sync_index = &cb_get_sync_index;
    s_hwIface.get_sync_index_mask = &cb_get_sync_index_mask;
    s_hwIface.set_command_buffers = &cb_set_command_buffers;
    s_hwIface.wait_sync_index = &cb_wait_sync_index;
    s_hwIface.lock_queue = &cb_lock_queue;
    s_hwIface.unlock_queue = &cb_unlock_queue;
    s_hwIface.set_signal_semaphore = &cb_set_signal_semaphore;
}

}  // namespace

// =========================================================================
// Public API
// =========================================================================

bool CreateInstance()
{
    VK_LOG_INFO("CreateInstance begin build=20260802-switchvk-wholearchive-v9");
#ifdef __SWITCH__
    s_nwindow = nwindowGetDefault();
#endif

    if (!CreateInstanceInternal())
        return false;
    VK_LOG_INFO("CreateInstanceInternal ok");
    if (!CreateSurfaceInternal())
        return false;
    VK_LOG_INFO("CreateSurfaceInternal ok");
    return true;
}

bool CreateDeviceAndSwapchain()
{
    VK_LOG_INFO("CreateDeviceAndSwapchain begin");
    if (!CreateDeviceInternal())
        return false;
    VK_LOG_INFO("CreateDeviceInternal ok");
    if (!CreateSwapchainInternal())
        return false;
    VK_LOG_INFO("CreateSwapchainInternal ok");
    if (!CreateOverlayRenderTargets())
        return false;
    VK_LOG_INFO("CreateOverlayRenderTargets ok");
    if (!CreateFrameResources())
        return false;
    VK_LOG_INFO("CreateFrameResources ok");
    PopulateHwInterface();
    s_ready = true;
    VK_LOG_INFO("CreateDeviceAndSwapchain ready");
    return true;
}

void Shutdown()
{
    if (s_device)
    {
        try { s_device.waitIdle(); } catch (...) {}
    }

    for (auto& runtime : s_slangRuntimes)
        DestroySlangRuntime(runtime);
    s_slangRuntimes.clear();
    s_activeSlangPreset = nullptr;
    s_activeSlangPresetVersion = 0;

    ShutdownOverlayRendererInternal();

    if (s_negIface && s_negIface->destroy_device)
        s_negIface->destroy_device();
    s_negIface = nullptr;

    for (auto& f : s_frames)
    {
        for (vk::Buffer buffer : f.deferredUploadBuffers)
            if (buffer) s_device.destroyBuffer(buffer);
        for (vk::DeviceMemory memory : f.deferredUploadMemories)
            if (memory) s_device.freeMemory(memory);
        if (f.thumbnailBuffer) s_device.destroyBuffer(f.thumbnailBuffer);
        if (f.thumbnailMemory) s_device.freeMemory(f.thumbnailMemory);
        if (f.inflightFence)   s_device.destroyFence(f.inflightFence);
        if (f.acquireSemaphore) s_device.destroySemaphore(f.acquireSemaphore);
        if (f.renderSemaphore)  s_device.destroySemaphore(f.renderSemaphore);
    }
    s_frames.clear();

    if (s_commandPool) s_device.destroyCommandPool(s_commandPool);
    s_commandPool = nullptr;

    DestroyOverlayRenderTargets();

    for (auto& v : s_swapImageViews)
        if (v) s_device.destroyImageView(v);
    s_swapImageViews.clear();
    s_swapImages.clear();

    if (s_swapchain) s_device.destroySwapchainKHR(s_swapchain);
    s_swapchain = nullptr;

    if (s_device) s_device.destroy();
    s_device = nullptr;

    if (s_surface) s_instance.destroySurfaceKHR(s_surface);
    s_surface = nullptr;

    if (s_instance) s_instance.destroy();
    s_instance = nullptr;

    s_ready = false;
    s_frameInFlight = false;
    s_lastImageValid = false;
}

bool BeginFrame()
{
    if (!s_ready)
        return false;

    // s_currentFrame is the rotating frame slot — it owns our per-frame sync
    // (fence + acquire semaphore + render semaphore + cmd buffer) AND it's
    // what we return to the libretro core via get_sync_index. The acquired
    // swap image index is independent and tracked in s_currentImage.
    if (s_frames.empty() || s_currentFrame >= s_frames.size())
    {
        VK_LOG_ERROR("BeginFrame has no frame resources");
        return false;
    }
    PerFrame& f = s_frames[s_currentFrame];

    // Wait for the previous use of this frame slot to complete on the GPU.
    (void)s_device.waitForFences(f.inflightFence, VK_TRUE, UINT64_MAX);
    CollectRetiredSlangRuntimes();
    CollectRetiredOverlayTextures();
    for (vk::Buffer buffer : f.deferredUploadBuffers)
        if (buffer) s_device.destroyBuffer(buffer);
    for (vk::DeviceMemory memory : f.deferredUploadMemories)
        if (memory) s_device.freeMemory(memory);
    f.deferredUploadBuffers.clear();
    f.deferredUploadMemories.clear();
    if (f.thumbnailBuffer) s_device.destroyBuffer(f.thumbnailBuffer);
    if (f.thumbnailMemory) s_device.freeMemory(f.thumbnailMemory);
    f.thumbnailBuffer = nullptr;
    f.thumbnailMemory = nullptr;
    f.thumbnailWidth = 0;
    f.thumbnailHeight = 0;
    f.thumbnailPending = false;

    try
    {
        auto rv = s_device.acquireNextImageKHR(s_swapchain, UINT64_MAX,
                                               f.acquireSemaphore, VK_NULL_HANDLE);
        s_currentImage = rv.value;
        if (rv.result == vk::Result::eSuboptimalKHR)
            VK_LOG_WARN("acquireNextImage suboptimal");
    }
    catch (const vk::OutOfDateKHRError&)
    {
        VK_LOG_WARN("acquireNextImage OUT_OF_DATE — TODO recreate swapchain");
        return false;
    }
    catch (const vk::SystemError& e)
    {
        VK_LOG_ERROR("acquireNextImage failed: %s", e.what());
        return false;
    }

    f.imageValid = false;
    f.coreCommandBuffers.clear();
    f.signalSemaphore = VK_NULL_HANDLE;

    // Begin our cmd buffer for the post-core composite + present transitions.
    f.cmd.reset();
    f.cmd.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

    s_frameInFlight = true;
    return true;
}

void EndFrame()
{
    if (!s_frameInFlight)
        return;

    if (s_frames.empty() || s_currentFrame >= s_frames.size() ||
        s_currentImage >= s_swapImages.size())
    {
        VK_LOG_ERROR("EndFrame received invalid frame=%u image=%u resources=%zu swapImages=%zu",
                     s_currentFrame, s_currentImage, s_frames.size(), s_swapImages.size());
        s_frameInFlight = false;
        return;
    }
    PerFrame& f = s_frames[s_currentFrame];
    vk::Image swapImage = s_swapImages[s_currentImage];
    const bool hasPendingOverlayUploads = !s_pendingOverlayUploads.empty();
    RecordPendingOverlayUploads(f);
    static uint32_t tracedFrames = 0;
    const bool trace = tracedFrames < 3;
    if (trace)
        VK_LOG_INFO("EndFrame[%u] begin slot=%u image=%u coreCmd=%zu imageValid=%d overlay=%d",
                    tracedFrames, s_currentFrame, s_currentImage, f.coreCommandBuffers.size(),
                    f.imageValid ? 1 : 0, s_overlayReady ? 1 : 0);

    const retro_vulkan_image* sourceImage = nullptr;
    bool reusingLastImage = false;
    if (f.imageValid && f.image.create_info.image != VK_NULL_HANDLE)
    {
        sourceImage = &f.image;
    }
    else if (s_lastImageValid && s_lastImage.create_info.image != VK_NULL_HANDLE)
    {
        sourceImage = &s_lastImage;
        reusingLastImage = true;
    }

    const bool hasOverlayDraw = s_overlayReady && s_overlayDrawData &&
        s_overlayDrawData->TotalVtxCount > 0 && s_currentImage < s_overlayFramebuffers.size();

    // Flycast boots through several frames before it calls set_image().  A
    // no-op frame only has to release the acquired image; submitting a clear
    // command buffer here was the first device queue submit and consistently
    // crashed switchVK before the core ever rendered.  Present can consume
    // the acquire semaphore directly, and the frame fence remains signalled
    // because this path has no GPU submission.
    if (!sourceImage && !hasOverlayDraw && !hasPendingOverlayUploads)
    {
        f.cmd.end();
        s_overlayDrawData = nullptr;
        vk::PresentInfoKHR present;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &f.acquireSemaphore;
        present.swapchainCount = 1;
        present.pSwapchains = &s_swapchain;
        present.pImageIndices = &s_currentImage;
        try
        {
            if (trace)
                VK_LOG_INFO("EndFrame[%u] boot frame direct present queue=%p", tracedFrames,
                            static_cast<VkQueue>(s_presentQueue));
            std::lock_guard<std::mutex> guard(s_queueMutex);
            (void)s_presentQueue.presentKHR(present);
        }
        catch (const vk::SystemError& e)
        {
            VK_LOG_ERROR("boot frame presentKHR failed: %s", e.what());
        }
        s_frameInFlight = false;
        s_thumbnailCaptureRequested = false;
        s_currentFrame = (s_currentFrame + 1) % static_cast<uint32_t>(s_frames.size());
        if (trace)
        {
            VK_LOG_INFO("EndFrame[%u] boot frame present complete", tracedFrames);
            ++tracedFrames;
        }
        return;
    }

    TransitionLayout(f.cmd, swapImage,
                     vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                     {}, vk::AccessFlagBits::eTransferWrite,
                     vk::PipelineStageFlagBits::eTopOfPipe,
                     vk::PipelineStageFlagBits::eTransfer);
    // The core blit is normally fullscreen, but it is not a valid guarantee
    // that every swapchain texel has been written in every frame (a core can
    // temporarily change its backing extent or output viewport).  The overlay
    // pass deliberately uses LOAD so that a transparent full-screen mask can
    // be blended above the game.  Therefore initialise the *entire* acquired
    // swap image before every blit, including the 16:9 path.  This prevents a
    // rotating swapchain image's undefined/old right-hand pixels from leaking
    // through semi-transparent mask texels as black flicker.
    {
        vk::ClearColorValue clear(std::array<float, 4>{0.f, 0.f, 0.f, 1.f});
        vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
        f.cmd.clearColorImage(swapImage, vk::ImageLayout::eTransferDstOptimal, clear, range);
        vk::MemoryBarrier clearToBlit(vk::AccessFlagBits::eTransferWrite,
                                      vk::AccessFlagBits::eTransferWrite);
        f.cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                              vk::PipelineStageFlagBits::eTransfer,
                              {}, clearToBlit, {}, {});
    }
    if (trace)
        VK_LOG_INFO("EndFrame[%u] swap transition and full clear complete", tracedFrames);

    if (sourceImage)
    {
        if (trace)
            VK_LOG_INFO("EndFrame[%u] source image=%p layout=%d reused=%d", tracedFrames,
                        sourceImage->create_info.image, static_cast<int>(sourceImage->image_layout),
                        reusingLastImage ? 1 : 0);
        if (reusingLastImage && s_lastImageFrame < s_frames.size() &&
            s_lastImageFrame != s_currentFrame && s_frames[s_lastImageFrame].inflightFence)
        {
            (void)s_device.waitForFences(s_frames[s_lastImageFrame].inflightFence,
                                         VK_TRUE, UINT64_MAX);
        }

        vk::Image coreImage(sourceImage->create_info.image);
        vk::ImageLayout coreLayout = static_cast<vk::ImageLayout>(sourceImage->image_layout);

        // Prefer the exact backing-image extent supplied by our core. The
        // AV/video_refresh extent is merely logical and becomes wrong when
        // Flycast renders above 480p.
        const auto* imageExtent = static_cast<const GBAStationImageExtent*>(
            sourceImage->create_info.pNext);
        const bool hasImageExtent = imageExtent &&
            imageExtent->magic == kGBAStationImageExtentMagic &&
            imageExtent->width > 0 && imageExtent->height > 0;
        const uint32_t srcW = hasImageExtent ? imageExtent->width :
            (s_sourceExtent.width ? s_sourceExtent.width : s_swapExtent.width);
        const uint32_t srcH = hasImageExtent ? imageExtent->height :
            (s_sourceExtent.height ? s_sourceExtent.height : s_swapExtent.height);
        SlangOutput slangOutput;
        const bool slangApplied = RenderSlangFrame(f, *sourceImage, {srcW, srcH}, slangOutput);
        if (s_activeSlangPreset)
            VK_LOG_INFO("Slang composite applied=%d output=%p extent=%ux%u presetVersion=%llu",
                        slangApplied ? 1 : 0, static_cast<VkImage>(slangOutput.image),
                        slangOutput.extent.width, slangOutput.extent.height,
                        static_cast<unsigned long long>(s_activeSlangPresetVersion));
        vk::Image blitSource = slangApplied ? slangOutput.image : coreImage;
        vk::ImageLayout blitSourceLayout = slangApplied ? vk::ImageLayout::eShaderReadOnlyOptimal : coreLayout;
        const uint32_t blitSourceW = slangApplied ? slangOutput.extent.width : srcW;
        const uint32_t blitSourceH = slangApplied ? slangOutput.extent.height : srcH;

        // A failed Slang chain deliberately falls back to the established
        // transfer path instead of leaving a black or partially rendered image.
        TransitionLayout(f.cmd, blitSource,
                         blitSourceLayout, vk::ImageLayout::eTransferSrcOptimal,
                         slangApplied ? vk::AccessFlagBits::eShaderRead :
                                        (vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite),
                         vk::AccessFlagBits::eTransferRead,
                         slangApplied ? vk::PipelineStageFlagBits::eFragmentShader : vk::PipelineStageFlagBits::eAllCommands,
                         vk::PipelineStageFlagBits::eTransfer);
        if (trace)
            VK_LOG_INFO("EndFrame[%u] source transition complete", tracedFrames);

        static uint32_t lastSrcW = 0;
        static uint32_t lastSrcH = 0;
        static uint32_t lastLogicalW = 0;
        static uint32_t lastLogicalH = 0;
        if (srcW != lastSrcW || srcH != lastSrcH ||
            s_sourceExtent.width != lastLogicalW || s_sourceExtent.height != lastLogicalH)
        {
            VK_LOG_INFO("blit source=%ux%u logical=%ux%u exact=%d", srcW, srcH,
                        s_sourceExtent.width, s_sourceExtent.height, hasImageExtent ? 1 : 0);
            lastSrcW = srcW;
            lastSrcH = srcH;
            lastLogicalW = s_sourceExtent.width;
            lastLogicalH = s_sourceExtent.height;
        }

        // Destination rect: the overlay's screen-size/display-mode selection,
        // or the full swapchain when none is set. The complete swapchain has
        // already been cleared above, so border bars remain deterministic.
        int32_t dstX0 = 0, dstY0 = 0;
        int32_t dstX1 = static_cast<int32_t>(s_swapExtent.width);
        int32_t dstY1 = static_cast<int32_t>(s_swapExtent.height);
        const bool letterbox =
            s_gameViewport.extent.width > 0 && s_gameViewport.extent.height > 0 &&
            (s_gameViewport.offset.x != 0 || s_gameViewport.offset.y != 0 ||
             s_gameViewport.extent.width != s_swapExtent.width ||
             s_gameViewport.extent.height != s_swapExtent.height);
        if (letterbox)
        {
            dstX0 = s_gameViewport.offset.x;
            dstY0 = s_gameViewport.offset.y;
            dstX1 = dstX0 + static_cast<int32_t>(s_gameViewport.extent.width);
            dstY1 = dstY0 + static_cast<int32_t>(s_gameViewport.extent.height);

        }

        vk::ImageBlit blit;
        blit.srcSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
        blit.srcSubresource.mipLevel = 0;
        blit.srcSubresource.baseArrayLayer = 0;
        blit.srcSubresource.layerCount = 1;
        blit.srcOffsets[0] = vk::Offset3D(0, 0, 0);
        blit.srcOffsets[1] = vk::Offset3D(static_cast<int32_t>(blitSourceW),
                                          static_cast<int32_t>(blitSourceH), 1);
        blit.dstSubresource = blit.srcSubresource;
        blit.dstOffsets[0] = vk::Offset3D(dstX0, dstY0, 0);
        blit.dstOffsets[1] = vk::Offset3D(dstX1, dstY1, 1);

        f.cmd.blitImage(blitSource, vk::ImageLayout::eTransferSrcOptimal,
                        swapImage, vk::ImageLayout::eTransferDstOptimal,
                        blit, vk::Filter::eLinear);
        if (trace)
            VK_LOG_INFO("EndFrame[%u] blit complete src=%ux%u dst=%d,%d-%d,%d", tracedFrames,
                        srcW, srcH, dstX0, dstY0, dstX1, dstY1);

        // Restore the source image layout. The Slang executor already put the
        // core image back in the layout owned by the libretro core.
        TransitionLayout(f.cmd, blitSource,
                         vk::ImageLayout::eTransferSrcOptimal, blitSourceLayout,
                         vk::AccessFlagBits::eTransferRead,
                         slangApplied ? vk::AccessFlagBits::eShaderRead :
                                        (vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite),
                         vk::PipelineStageFlagBits::eTransfer,
                         slangApplied ? vk::PipelineStageFlagBits::eFragmentShader : vk::PipelineStageFlagBits::eAllCommands);
        if (trace)
            VK_LOG_INFO("EndFrame[%u] source restore complete", tracedFrames);
    }
    else
    {
        // Core didn't produce a frame. The unconditional full clear above
        // already made this acquired swap image deterministic.
        if (trace)
            VK_LOG_INFO("EndFrame[%u] no core image; retaining full clear", tracedFrames);
    }

    // The menu-opening frame has no overlay draw data. Copy the completed game
    // composite while this command buffer still owns the swap image in
    // TRANSFER_DST. Unlike the old post-present path, this never transitions
    // or submits a swapchain image after VI owns it.
    if (s_thumbnailCaptureRequested)
    {
        try
        {
            const vk::DeviceSize bytes = static_cast<vk::DeviceSize>(s_swapExtent.width) * s_swapExtent.height * 4;
            vk::BufferCreateInfo info;
            info.size = bytes;
            info.usage = vk::BufferUsageFlagBits::eTransferDst;
            info.sharingMode = vk::SharingMode::eExclusive;
            f.thumbnailBuffer = s_device.createBuffer(info);
            const vk::MemoryRequirements requirements = s_device.getBufferMemoryRequirements(f.thumbnailBuffer);
            uint32_t memoryType = 0;
            if (!FindMemoryType(requirements.memoryTypeBits,
                                vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
                                memoryType))
                throw std::runtime_error("thumbnail staging memory unavailable");
            vk::MemoryAllocateInfo allocation(requirements.size, memoryType);
            f.thumbnailMemory = s_device.allocateMemory(allocation);
            s_device.bindBufferMemory(f.thumbnailBuffer, f.thumbnailMemory, 0);
            vk::BufferImageCopy region;
            region.imageSubresource = vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
            region.imageExtent = vk::Extent3D(s_swapExtent.width, s_swapExtent.height, 1);
            f.cmd.copyImageToBuffer(swapImage, vk::ImageLayout::eTransferDstOptimal, f.thumbnailBuffer, region);
            f.thumbnailWidth = s_swapExtent.width;
            f.thumbnailHeight = s_swapExtent.height;
            f.thumbnailPending = true;
            s_thumbnailCaptureFrame = static_cast<int>(s_currentFrame);
        }
        catch (const std::exception &e)
        {
            VK_LOG_ERROR("State thumbnail copy setup failed: %s", e.what());
            if (f.thumbnailBuffer) s_device.destroyBuffer(f.thumbnailBuffer);
            if (f.thumbnailMemory) s_device.freeMemory(f.thumbnailMemory);
            f.thumbnailBuffer = nullptr;
            f.thumbnailMemory = nullptr;
            f.thumbnailPending = false;
            s_thumbnailCaptureFrame = -1;
        }
        s_thumbnailCaptureRequested = false;
    }

    if (hasOverlayDraw)
    {
        TransitionLayout(f.cmd, swapImage,
                         vk::ImageLayout::eTransferDstOptimal,
                         vk::ImageLayout::eColorAttachmentOptimal,
                         vk::AccessFlagBits::eTransferWrite,
                         // ImGui's menu, dimmer and full-screen mask use
                         // alpha blending. Blending reads the destination
                         // colour before it writes the result, so a write-only
                         // destination mask leaves the preceding game blit
                         // unavailable to the blend unit on some frames.
                         vk::AccessFlagBits::eColorAttachmentRead |
                         vk::AccessFlagBits::eColorAttachmentWrite,
                         vk::PipelineStageFlagBits::eTransfer,
                         vk::PipelineStageFlagBits::eColorAttachmentOutput);

        vk::RenderPassBeginInfo rpbi;
        rpbi.renderPass = s_overlayRenderPass;
        rpbi.framebuffer = s_overlayFramebuffers[s_currentImage];
        rpbi.renderArea.offset = vk::Offset2D(0, 0);
        rpbi.renderArea.extent = s_swapExtent;
        f.cmd.beginRenderPass(rpbi, vk::SubpassContents::eInline);
        ImGui_ImplVulkan_RenderDrawData(s_overlayDrawData, static_cast<VkCommandBuffer>(f.cmd));
        f.cmd.endRenderPass();
        if (trace)
            VK_LOG_INFO("EndFrame[%u] overlay render complete", tracedFrames);

        TransitionLayout(f.cmd, swapImage,
                         vk::ImageLayout::eColorAttachmentOptimal,
                         vk::ImageLayout::ePresentSrcKHR,
                         vk::AccessFlagBits::eColorAttachmentWrite, {},
                         vk::PipelineStageFlagBits::eColorAttachmentOutput,
                         vk::PipelineStageFlagBits::eBottomOfPipe);
    }
    else
    {
        // Transition swap image to PRESENT_SRC.
        TransitionLayout(f.cmd, swapImage,
                         vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::ePresentSrcKHR,
                         vk::AccessFlagBits::eTransferWrite, {},
                         vk::PipelineStageFlagBits::eTransfer,
                         vk::PipelineStageFlagBits::eBottomOfPipe);
    }
    s_overlayDrawData = nullptr;

    f.cmd.end();
    if (trace)
        VK_LOG_INFO("EndFrame[%u] command buffer end complete", tracedFrames);

    // Submit: any core-supplied cmd buffers run first, then ours.
    std::vector<vk::CommandBuffer> submitCmds;
    submitCmds.reserve(f.coreCommandBuffers.size() + 1);
    for (auto& cb : f.coreCommandBuffers)
        submitCmds.push_back(cb);
    submitCmds.push_back(f.cmd);

    // The acquired image is first transitioned, cleared, and blitted by transfer
    // commands.  Waiting at color-attachment output lets those commands race the
    // presentation engine on the first frame.
    vk::PipelineStageFlags waitStage = vk::PipelineStageFlagBits::eTransfer;

    std::vector<vk::Semaphore> signalSems = {f.renderSemaphore};
    if (f.signalSemaphore != VK_NULL_HANDLE)
        signalSems.push_back(vk::Semaphore(f.signalSemaphore));

    vk::SubmitInfo submit;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &f.acquireSemaphore;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = static_cast<uint32_t>(submitCmds.size());
    submit.pCommandBuffers = submitCmds.data();
    submit.signalSemaphoreCount = static_cast<uint32_t>(signalSems.size());
    submit.pSignalSemaphores = signalSems.data();

    {
        if (trace)
            VK_LOG_INFO("EndFrame[%u] submit begin cmds=%u signals=%u", tracedFrames,
                        submit.commandBufferCount, submit.signalSemaphoreCount);
        std::lock_guard<std::mutex> guard(s_queueMutex);
        s_device.resetFences(f.inflightFence);
        s_queue.submit(submit, f.inflightFence);
    }
    if (trace)
        VK_LOG_INFO("EndFrame[%u] submit complete", tracedFrames);

    // Present the acquired swap image (index returned by acquireNextImageKHR,
    // not the rotating frame slot).
    vk::PresentInfoKHR present;
    present.waitSemaphoreCount = 1;
    f.presentedImage = s_currentImage;
    present.pWaitSemaphores = &f.renderSemaphore;
    present.swapchainCount = 1;
    present.pSwapchains = &s_swapchain;
    present.pImageIndices = &s_currentImage;

    try
    {
        if (trace)
            VK_LOG_INFO("EndFrame[%u] present call queue=%p", tracedFrames,
                        static_cast<VkQueue>(s_presentQueue));
        std::lock_guard<std::mutex> guard(s_queueMutex);
        (void)s_presentQueue.presentKHR(present);
    }
    catch (const vk::OutOfDateKHRError&)
    {
        VK_LOG_WARN("presentKHR OUT_OF_DATE — TODO recreate swapchain");
    }
    catch (const vk::SystemError& e)
    {
        VK_LOG_ERROR("presentKHR failed: %s", e.what());
    }

    s_frameInFlight = false;
    s_currentFrame = (s_currentFrame + 1) % static_cast<uint32_t>(s_frames.size());
    if (trace)
    {
        VK_LOG_INFO("EndFrame[%u] present complete", tracedFrames);
        ++tracedFrames;
    }
}

void RequestCurrentFrameCapture()
{
    if (s_frameInFlight)
        s_thumbnailCaptureRequested = true;
}

bool ConsumeCurrentFrameCaptureRGBA(std::vector<uint8_t>& out, uint32_t& width, uint32_t& height)
{
    out.clear();
    width = height = 0;
    if (!s_device || s_thumbnailCaptureFrame < 0 ||
        s_thumbnailCaptureFrame >= static_cast<int>(s_frames.size()))
        return false;
    PerFrame &frame = s_frames[static_cast<size_t>(s_thumbnailCaptureFrame)];
    if (!frame.thumbnailPending || !frame.thumbnailBuffer || !frame.thumbnailMemory ||
        frame.thumbnailWidth == 0 || frame.thumbnailHeight == 0)
        return false;
    try
    {
        (void)s_device.waitForFences(frame.inflightFence, VK_TRUE, UINT64_MAX);
        const vk::DeviceSize bytes = static_cast<vk::DeviceSize>(frame.thumbnailWidth) * frame.thumbnailHeight * 4;
        void *mapped = s_device.mapMemory(frame.thumbnailMemory, 0, bytes);
        out.resize(static_cast<size_t>(bytes));
        std::memcpy(out.data(), mapped, out.size());
        s_device.unmapMemory(frame.thumbnailMemory);
        // The Switch swapchain format is BGRA. PNG generation consumes RGBA.
        if (s_swapFormat == vk::Format::eB8G8R8A8Unorm)
            for (size_t i = 0; i < out.size(); i += 4)
                std::swap(out[i], out[i + 2]);
        width = frame.thumbnailWidth;
        height = frame.thumbnailHeight;
        s_device.destroyBuffer(frame.thumbnailBuffer);
        s_device.freeMemory(frame.thumbnailMemory);
        frame.thumbnailBuffer = nullptr;
        frame.thumbnailMemory = nullptr;
        frame.thumbnailWidth = frame.thumbnailHeight = 0;
        frame.thumbnailPending = false;
        s_thumbnailCaptureFrame = -1;
        VK_LOG_INFO("State thumbnail captured %ux%u from in-frame staging copy", width, height);
        return true;
    }
    catch (const vk::SystemError &e)
    {
        VK_LOG_ERROR("State thumbnail staging read failed: %s", e.what());
        return false;
    }
}

bool IsFrameInFlight() { return s_frameInFlight; }
bool IsReady()         { return s_ready; }

bool InitOverlayRenderer()
{
    if (s_overlayReady)
        return true;
    if (!s_ready || !s_device || !s_overlayRenderPass)
    {
        VK_LOG_WARN("Overlay renderer init skipped: Vulkan not ready");
        return false;
    }

    try
    {
        vk::DescriptorPoolSize poolSize(vk::DescriptorType::eCombinedImageSampler, 64);
        vk::DescriptorPoolCreateInfo dpci;
        dpci.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
        dpci.maxSets = 64;
        dpci.poolSizeCount = 1;
        dpci.pPoolSizes = &poolSize;
        s_overlayDescriptorPool = s_device.createDescriptorPool(dpci);
    }
    catch (const vk::SystemError& e)
    {
        VK_LOG_ERROR("Overlay descriptor pool creation failed: %s", e.what());
        return false;
    }

#ifdef IMGUI_IMPL_VULKAN_NO_PROTOTYPES
    if (!ImGui_ImplVulkan_LoadFunctions(ImGuiVulkanLoader, nullptr))
    {
        VK_LOG_ERROR("ImGui Vulkan function loading failed");
        ShutdownOverlayRendererInternal();
        return false;
    }
#endif

    ImGui_ImplVulkan_InitInfo info{};
    info.Instance = static_cast<VkInstance>(s_instance);
    info.PhysicalDevice = static_cast<VkPhysicalDevice>(s_gpu);
    info.Device = static_cast<VkDevice>(s_device);
    info.QueueFamily = s_queueFamilyIndex;
    info.Queue = static_cast<VkQueue>(s_queue);
    info.DescriptorPool = static_cast<VkDescriptorPool>(s_overlayDescriptorPool);
    info.RenderPass = static_cast<VkRenderPass>(s_overlayRenderPass);
    info.MinImageCount = 2;
    info.ImageCount = static_cast<uint32_t>(s_swapImages.size());
    info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

    if (!ImGui_ImplVulkan_Init(&info))
    {
        VK_LOG_ERROR("ImGui_ImplVulkan_Init failed");
        ShutdownOverlayRendererInternal();
        return false;
    }
    s_overlayReady = true;

    if (!ImGui_ImplVulkan_CreateFontsTexture())
    {
        VK_LOG_ERROR("ImGui_ImplVulkan_CreateFontsTexture failed");
        ShutdownOverlayRendererInternal();
        return false;
    }

    VK_LOG_INFO("Overlay renderer initialized");
    return true;
}

void ShutdownOverlayRenderer()
{
    if (s_device)
        try { s_device.waitIdle(); } catch (...) {}
    ShutdownOverlayRendererInternal();
}

void BeginOverlayFrame()
{
    if (s_overlayReady)
        ImGui_ImplVulkan_NewFrame();
}

void SetOverlayDrawData(ImDrawData* drawData)
{
    s_overlayDrawData = drawData;
}

void SetSlangPreset(const GBAStationSlang::Preset* preset, uint64_t version)
{
    if (s_activeSlangPreset != preset || s_activeSlangPresetVersion != version)
        s_slangCleanupRequested = true;
    s_activeSlangPreset = preset;
    s_activeSlangPresetVersion = version;
}

ImTextureID CreateOverlayTextureRGBA(const unsigned char* rgba, uint32_t width, uint32_t height)
{
    if (!s_overlayReady || !s_device || !s_commandPool || !rgba || width == 0 || height == 0)
        return 0;

    const vk::DeviceSize uploadSize = static_cast<vk::DeviceSize>(width) *
                                      static_cast<vk::DeviceSize>(height) * 4;
    OverlayTextureResource texture{};
    vk::Buffer stagingBuffer;
    vk::DeviceMemory stagingMemory;

    try
    {
        vk::BufferCreateInfo bufferInfo;
        bufferInfo.size = uploadSize;
        bufferInfo.usage = vk::BufferUsageFlagBits::eTransferSrc;
        bufferInfo.sharingMode = vk::SharingMode::eExclusive;
        stagingBuffer = s_device.createBuffer(bufferInfo);

        vk::MemoryRequirements bufferReq = s_device.getBufferMemoryRequirements(stagingBuffer);
        uint32_t bufferMemoryType = 0;
        if (!FindMemoryType(bufferReq.memoryTypeBits,
                            vk::MemoryPropertyFlagBits::eHostVisible |
                            vk::MemoryPropertyFlagBits::eHostCoherent,
                            bufferMemoryType) &&
            !FindMemoryType(bufferReq.memoryTypeBits,
                            vk::MemoryPropertyFlagBits::eHostVisible,
                            bufferMemoryType))
        {
            VK_LOG_ERROR("No host-visible memory type for overlay texture upload");
            throw std::runtime_error("overlay texture staging memory");
        }

        vk::MemoryAllocateInfo bufferAlloc;
        bufferAlloc.allocationSize = bufferReq.size;
        bufferAlloc.memoryTypeIndex = bufferMemoryType;
        stagingMemory = s_device.allocateMemory(bufferAlloc);
        s_device.bindBufferMemory(stagingBuffer, stagingMemory, 0);

        void* mapped = s_device.mapMemory(stagingMemory, 0, uploadSize);
        std::memcpy(mapped, rgba, static_cast<size_t>(uploadSize));
        vk::MappedMemoryRange flushRange;
        flushRange.memory = stagingMemory;
        flushRange.size = uploadSize;
        s_device.flushMappedMemoryRanges(flushRange);
        s_device.unmapMemory(stagingMemory);

        vk::ImageCreateInfo imageInfo;
        imageInfo.imageType = vk::ImageType::e2D;
        imageInfo.format = vk::Format::eR8G8B8A8Unorm;
        imageInfo.extent = vk::Extent3D(width, height, 1);
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = vk::SampleCountFlagBits::e1;
        imageInfo.tiling = vk::ImageTiling::eOptimal;
        imageInfo.usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
        imageInfo.sharingMode = vk::SharingMode::eExclusive;
        imageInfo.initialLayout = vk::ImageLayout::eUndefined;
        texture.image = s_device.createImage(imageInfo);

        vk::MemoryRequirements imageReq = s_device.getImageMemoryRequirements(texture.image);
        uint32_t imageMemoryType = 0;
        if (!FindMemoryType(imageReq.memoryTypeBits,
                            vk::MemoryPropertyFlagBits::eDeviceLocal,
                            imageMemoryType))
        {
            VK_LOG_ERROR("No device-local memory type for overlay texture");
            throw std::runtime_error("overlay texture image memory");
        }

        vk::MemoryAllocateInfo imageAlloc;
        imageAlloc.allocationSize = imageReq.size;
        imageAlloc.memoryTypeIndex = imageMemoryType;
        texture.memory = s_device.allocateMemory(imageAlloc);
        s_device.bindImageMemory(texture.image, texture.memory, 0);

        vk::ImageViewCreateInfo viewInfo;
        viewInfo.image = texture.image;
        viewInfo.viewType = vk::ImageViewType::e2D;
        viewInfo.format = vk::Format::eR8G8B8A8Unorm;
        viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        texture.view = s_device.createImageView(viewInfo);

        vk::SamplerCreateInfo samplerInfo;
        samplerInfo.magFilter = vk::Filter::eLinear;
        samplerInfo.minFilter = vk::Filter::eLinear;
        samplerInfo.mipmapMode = vk::SamplerMipmapMode::eLinear;
        samplerInfo.addressModeU = vk::SamplerAddressMode::eClampToEdge;
        samplerInfo.addressModeV = vk::SamplerAddressMode::eClampToEdge;
        samplerInfo.addressModeW = vk::SamplerAddressMode::eClampToEdge;
        samplerInfo.minLod = 0.0f;
        samplerInfo.maxLod = 0.0f;
        texture.sampler = s_device.createSampler(samplerInfo);

        texture.descriptor = ImGui_ImplVulkan_AddTexture(static_cast<VkSampler>(texture.sampler),
                                                         static_cast<VkImageView>(texture.view),
                                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        s_overlayTextures.push_back(texture);
        s_pendingOverlayUploads.push_back({stagingBuffer, stagingMemory, texture.image, width, height});
        stagingBuffer = nullptr;
        stagingMemory = nullptr;
        VK_LOG_INFO("Overlay texture queued for frame upload: %ux%u", width, height);
        return (ImTextureID)texture.descriptor;
    }
    catch (const vk::SystemError& e)
    {
        VK_LOG_ERROR("Overlay texture creation failed: %s", e.what());
    }
    catch (const std::exception& e)
    {
        VK_LOG_ERROR("Overlay texture creation failed: %s", e.what());
    }

    if (stagingBuffer) s_device.destroyBuffer(stagingBuffer);
    if (stagingMemory) s_device.freeMemory(stagingMemory);
    DestroyOverlayTextureResource(texture, false);
    return 0;
}

void DestroyOverlayTexture(ImTextureID textureId)
{
    if (!textureId || !s_device)
        return;

    const VkDescriptorSet descriptor = reinterpret_cast<VkDescriptorSet>(static_cast<uintptr_t>(textureId));
    const auto found = std::find_if(s_overlayTextures.begin(), s_overlayTextures.end(),
                                    [descriptor](const OverlayTextureResource& texture) {
                                        return texture.descriptor == descriptor;
                                    });
    if (found == s_overlayTextures.end()) return;
    // Never tear down an ImGui descriptor during the frame that may still use
    // it. BeginFrame later waits all front-end fences, then reclaims it.
    s_retiredOverlayTextures.push_back(std::move(*found));
    s_overlayTextures.erase(found);
    VK_LOG_INFO("Overlay texture retired; deferred until all frame fences signal");
}

const retro_hw_render_interface_vulkan* GetHwRenderInterface()
{
    return s_ready ? &s_hwIface : nullptr;
}

void SetNegotiationInterface(const retro_hw_render_context_negotiation_interface_vulkan* iface)
{
    s_negIface = iface;
}

void GetSwapExtent(uint32_t& width, uint32_t& height)
{
    width = s_swapExtent.width;
    height = s_swapExtent.height;
}

void SetSourceExtent(uint32_t width, uint32_t height)
{
    s_sourceExtent.width = width;
    s_sourceExtent.height = height;
}

void SetGameViewport(int x, int y, int width, int height)
{
    if (width <= 0 || height <= 0)
        s_gameViewport = vk::Rect2D{{0, 0}, {0, 0}};  // 0 extent => full swapchain
    else
        s_gameViewport = vk::Rect2D{{x, y}, {static_cast<uint32_t>(width),
                                             static_cast<uint32_t>(height)}};
}

bool CaptureCurrentFrameRGBA(std::vector<uint8_t>& out, uint32_t& width, uint32_t& height)
{
    if (!s_ready || !s_device || s_swapchain == VK_NULL_HANDLE || s_swapImages.empty())
        return false;

    try
    {
        const uint32_t imageCount = static_cast<uint32_t>(s_swapImages.size());
        // Wait for the most recently submitted frame so its blit is complete,
        // then read back the image that frame presented.
        const uint32_t prevFrame = (s_currentFrame + s_frames.size() - 1) % static_cast<uint32_t>(s_frames.size());
        (void)s_device.waitForFences(s_frames[prevFrame].inflightFence, VK_TRUE, UINT64_MAX);

        // swap images are indexed in acquire order; the previous present used
        // s_currentImage of that frame, which we can no longer recover exactly.
        // Read the image that was presented two acquires ago (guaranteed idle).
        const uint32_t srcImage = s_frames[prevFrame].presentedImage;
        vk::Image image = s_swapImages[srcImage];

        const uint32_t w = s_swapExtent.width;
        const uint32_t h = s_swapExtent.height;
        if (w == 0 || h == 0)
            return false;

        const vk::DeviceSize bufSize = static_cast<vk::DeviceSize>(w) * h * 4;

        vk::BufferCreateInfo bci;
        bci.size = bufSize;
        bci.usage = vk::BufferUsageFlagBits::eTransferDst;
        bci.sharingMode = vk::SharingMode::eExclusive;
        vk::Buffer staging = s_device.createBuffer(bci);

        vk::MemoryRequirements memReqs = s_device.getBufferMemoryRequirements(staging);
        vk::MemoryAllocateInfo mai;
        mai.allocationSize = memReqs.size;
        uint32_t memTypeIndex = 0;
        if (!FindMemoryType(memReqs.memoryTypeBits,
                            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
                            memTypeIndex))
        {
            s_device.destroyBuffer(staging);
            return false;
        }
        mai.memoryTypeIndex = memTypeIndex;
        vk::DeviceMemory mem = s_device.allocateMemory(mai);
        s_device.bindBufferMemory(staging, mem, 0);

        vk::CommandPoolCreateInfo cpci;
        cpci.flags = vk::CommandPoolCreateFlagBits::eTransient;
        cpci.queueFamilyIndex = s_queueFamilyIndex;
        vk::CommandPool pool = s_device.createCommandPool(cpci);
        vk::CommandBufferAllocateInfo cbai;
        cbai.commandPool = pool;
        cbai.level = vk::CommandBufferLevel::ePrimary;
        cbai.commandBufferCount = 1;
        vk::CommandBuffer cmd = s_device.allocateCommandBuffers(cbai)[0];

        cmd.begin(vk::CommandBufferBeginInfo());
        // Transition: PRESENT_SRC -> TRANSFER_SRC.
        vk::ImageMemoryBarrier barrier;
        barrier.image = image;
        barrier.oldLayout = vk::ImageLayout::ePresentSrcKHR;
        barrier.newLayout = vk::ImageLayout::eTransferSrcOptimal;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange = vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
        barrier.srcAccessMask = {};
        barrier.dstAccessMask = vk::AccessFlagBits::eTransferRead;
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eBottomOfPipe,
                            vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, barrier);

        vk::BufferImageCopy region;
        region.imageSubresource = vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
        region.imageExtent = vk::Extent3D(w, h, 1);
        cmd.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, staging, region);

        // Transition back to PRESENT_SRC.
        vk::ImageMemoryBarrier back = barrier;
        back.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
        back.newLayout = vk::ImageLayout::ePresentSrcKHR;
        back.srcAccessMask = vk::AccessFlagBits::eTransferRead;
        back.dstAccessMask = {};
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                            vk::PipelineStageFlagBits::eBottomOfPipe, {}, {}, {}, back);
        cmd.end();

        vk::Fence fence = s_device.createFence(vk::FenceCreateInfo());
        vk::SubmitInfo si;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        std::lock_guard<std::mutex> guard(s_queueMutex);
        s_queue.submit(1, &si, fence);
        (void)s_device.waitForFences(fence, VK_TRUE, UINT64_MAX);
        s_device.destroyFence(fence);

        out.resize(static_cast<std::size_t>(bufSize));
        void* mapped = s_device.mapMemory(mem, 0, bufSize);
        std::memcpy(out.data(), mapped, static_cast<std::size_t>(bufSize));
        s_device.unmapMemory(mem);

        s_device.freeCommandBuffers(pool, cmd);
        s_device.destroyCommandPool(pool);
        s_device.freeMemory(mem);
        s_device.destroyBuffer(staging);

        // Swapchain format is B8G8R8A8 (checked at creation); convert to RGBA8.
        if (s_swapFormat == vk::Format::eB8G8R8A8Unorm)
        {
            for (std::size_t i = 0; i < out.size(); i += 4)
                std::swap(out[i], out[i + 2]);
        }
        width = w;
        height = h;
        return true;
    }
    catch (const vk::SystemError& e)
    {
        VK_LOG_ERROR("CaptureCurrentFrameRGBA failed: %s", e.what());
        return false;
    }
}

}  // namespace GBAStationVulkan
