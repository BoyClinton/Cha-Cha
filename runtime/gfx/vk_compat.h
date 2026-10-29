// Vulkan 1.2 devices (older GPUs, e.g. GCN 1 Radeons on AMD's last Windows driver): the renderer is written against
// Vulkan 1.3's dynamic rendering and synchronization2. These calls take the core entry points on 1.3 devices, the KHR
// ones where a 1.2 driver has the extensions, and, where VK_KHR_dynamic_rendering is missing, emulate dynamic
// rendering with render passes and framebuffers built from the same VkRenderingInfo (cached; one device per process).
#pragma once

#include <vulkan/vulkan.h>

namespace mh3u::gfx::vkc {

// After vkCreateDevice: `dynamic_rendering` = core 1.3 or VK_KHR_dynamic_rendering enabled, `core13` = device API 1.3+.
void init(VkDevice device, bool core13, bool dynamic_rendering);
// Before vkDestroyDevice: frees the render passes and framebuffers.
void shutdown();
bool dynamic_rendering();

// vkCmdBeginRendering. The attachments' formats are passed alongside (a render pass needs them; VkRenderingInfo has
// only views): color_formats[i] for pColorAttachments[i], depth_format for the depth / stencil attachment.
void cmd_begin_rendering(VkCommandBuffer cmd, const VkRenderingInfo& info, const VkFormat* color_formats,
                         VkFormat depth_format = VK_FORMAT_UNDEFINED);
void cmd_end_rendering(VkCommandBuffer cmd);
void cmd_pipeline_barrier2(VkCommandBuffer cmd, const VkDependencyInfo& info);

// A render pass compatible with `rendering` (for pipelines made elsewhere, e.g. Dear ImGui's) where dynamic rendering
// is emulated; VK_NULL_HANDLE otherwise. Owned here.
VkRenderPass render_pass_for(const VkPipelineRenderingCreateInfo& rendering);
// For vkCreateGraphicsPipelines: sets pci.renderPass to a render pass compatible with `rendering` when dynamic
// rendering is emulated, and takes `rendering` out of the pNext chain then (it must be pci.pNext). No-op otherwise.
void pipeline_rendering(VkGraphicsPipelineCreateInfo& pci, const VkPipelineRenderingCreateInfo& rendering);

// Before vkDestroyImageView of a view used as an attachment: drops the framebuffers that refer to it.
void forget_view(VkImageView view);

}  // namespace mh3u::gfx::vkc
