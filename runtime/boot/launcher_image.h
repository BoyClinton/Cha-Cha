#pragma once
// Pictures in the launcher (quest and pack images in MODS -> QUESTS): decoded by Windows (WIC: png, jpg, webp, bmp)
// and uploaded as ImGui textures on the device shared with the renderer.
#include <imgui.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <filesystem>
#include <vector>

#include "presenter.h"

namespace mh3u::boot {

bool decode_image(const std::filesystem::path& file, std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h);

struct LauncherImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    float w = 0, h = 0;
    ImTextureID id() const { return static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(set)); }
};

// RGBA8 pixels -> a sampled image (one blocking upload on the shared queue). Needs the ImGui Vulkan backend.
bool upload_image(const gfx::VulkanContext& vk, const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h, VkSampler sampler,
                  LauncherImage& out);
// After the GPU is done with it (the caller waits for the device or a frame).
void destroy_image(VkDevice dev, LauncherImage& img);

}  // namespace mh3u::boot
