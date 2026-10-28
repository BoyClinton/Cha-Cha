#include "launcher_image.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#else
#include <SDL3/SDL.h>
#endif

#include <imgui_impl_vulkan.h>

#include <cstring>
#include <initializer_list>
#include <mutex>

namespace mh3u::boot {
namespace {

uint32_t memory_type(VkPhysicalDevice phys, uint32_t bits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    return UINT32_MAX;
}

}  // namespace

#ifndef _WIN32
// PNG through SDL (the quest and pack pictures are PNG files); WIC on Windows also reads JPEG
bool decode_image(const std::filesystem::path& file, std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h) {
    const auto u = file.u8string();
    SDL_Surface* png = SDL_LoadPNG(reinterpret_cast<const char*>(u.c_str()));
    if (!png) return false;
    SDL_Surface* conv = SDL_ConvertSurface(png, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(png);
    const bool ok = conv && conv->w > 0 && conv->h > 0 && conv->w <= 8192 && conv->h <= 8192;
    if (ok) {
        w = static_cast<uint32_t>(conv->w);
        h = static_cast<uint32_t>(conv->h);
        rgba.resize(size_t{w} * h * 4);
        for (uint32_t y = 0; y < h; ++y)
            std::memcpy(&rgba[size_t{y} * w * 4], static_cast<const uint8_t*>(conv->pixels) + size_t{y} * static_cast<size_t>(conv->pitch), size_t{w} * 4);
    }
    SDL_DestroySurface(conv);
    return ok;
}
#else
bool decode_image(const std::filesystem::path& file, std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    UINT uw = 0, uh = 0;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
              SUCCEEDED(factory->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) &&
              SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&conv)) &&
              SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
              SUCCEEDED(conv->GetSize(&uw, &uh)) && uw && uh && uw <= 8192 && uh <= 8192;
    if (ok) {
        w = uw;
        h = uh;
        rgba.resize(size_t{w} * h * 4);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(rgba.size()), rgba.data()));
    }
    for (IUnknown* u : std::initializer_list<IUnknown*>{conv, frame, decoder, factory})
        if (u) u->Release();
    if (SUCCEEDED(init)) CoUninitialize();
    return ok;
}
#endif

bool upload_image(const gfx::VulkanContext& vk, const std::vector<uint8_t>& px, uint32_t w, uint32_t h, VkSampler sampler, LauncherImage& t) {
    VkDevice dev = vk.device;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {w, h, 1};
    ici.mipLevels = ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (vkCreateImage(dev, &ici, nullptr, &t.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, t.image, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memory_type(vk.phys, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev, &mai, nullptr, &t.mem) != VK_SUCCESS) {
        destroy_image(dev, t);
        return false;
    }
    vkBindImageMemory(dev, t.image, t.mem, 0);
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = px.size();
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    vkCreateBuffer(dev, &bci, nullptr, &staging);
    vkGetBufferMemoryRequirements(dev, staging, &req);
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memory_type(vk.phys, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev, &mai, nullptr, &staging_mem) != VK_SUCCESS) {
        vkDestroyBuffer(dev, staging, nullptr);
        destroy_image(dev, t);
        return false;
    }
    vkBindBufferMemory(dev, staging, staging_mem, 0);
    void* map = nullptr;
    vkMapMemory(dev, staging_mem, 0, VK_WHOLE_SIZE, 0, &map);
    std::memcpy(map, px.data(), px.size());
    vkUnmapMemory(dev, staging_mem);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = vk.queue_family;
    VkCommandPool pool = VK_NULL_HANDLE;
    vkCreateCommandPool(dev, &pci, nullptr, &pool);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(dev, &cai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, staging, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    vkEndCommandBuffer(cmd);
    // a fence of its own: the queue is shared with the running game, so waiting for the whole queue is not needed
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(dev, &fci, nullptr, &fence);
    {
        std::lock_guard g(*vk.queue_mutex);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vkQueueSubmit(vk.queue, 1, &si, fence);
    }
    vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(dev, fence, nullptr);
    vkDestroyCommandPool(dev, pool, nullptr);
    vkDestroyBuffer(dev, staging, nullptr);
    vkFreeMemory(dev, staging_mem, nullptr);
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = t.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(dev, &vci, nullptr, &t.view);
    t.set = ImGui_ImplVulkan_AddTexture(sampler, t.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    t.w = static_cast<float>(w);
    t.h = static_cast<float>(h);
    return true;
}

void destroy_image(VkDevice dev, LauncherImage& t) {
    if (t.set) ImGui_ImplVulkan_RemoveTexture(t.set);
    if (t.view) vkDestroyImageView(dev, t.view, nullptr);
    if (t.image) vkDestroyImage(dev, t.image, nullptr);
    if (t.mem) vkFreeMemory(dev, t.mem, nullptr);
    t = {};
}

}  // namespace mh3u::boot
