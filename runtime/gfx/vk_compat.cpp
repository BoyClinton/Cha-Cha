#include "vk_compat.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <vector>

namespace mh3u::gfx::vkc {
namespace {

struct State {
    VkDevice device = VK_NULL_HANDLE;
    bool dynamic = true;
    PFN_vkCmdBeginRendering begin = nullptr;
    PFN_vkCmdEndRendering end = nullptr;
    PFN_vkCmdPipelineBarrier2 barrier2 = nullptr;
    std::mutex m;
    std::map<std::vector<uint64_t>, VkRenderPass> passes;       // attachment formats, ops and layouts
    std::map<std::vector<uint64_t>, VkFramebuffer> framebuffers;  // render pass, size, views
} g;

template <class F>
F proc(VkDevice dev, bool core13, const char* core, const char* khr) {
    PFN_vkVoidFunction f = core13 ? vkGetDeviceProcAddr(dev, core) : nullptr;
    if (!f) f = vkGetDeviceProcAddr(dev, khr);
    return reinterpret_cast<F>(f);
}

// One attachment of a render pass: format, then loadOp, storeOp, stencilLoadOp, stencilStoreOp, layout.
struct Att {
    VkFormat format;
    VkAttachmentLoadOp load, stencil_load;
    VkAttachmentStoreOp store, stencil_store;
    VkImageLayout layout;
};

// Render pass with one subpass: `colors` (VK_FORMAT_UNDEFINED = unused slot), then an optional depth / stencil one.
// Layouts stay as given (initial = final = the attachment layout), as with dynamic rendering. Caller holds g.m.
VkRenderPass render_pass(const std::vector<Att>& colors, const Att* depth) {
    std::vector<uint64_t> key;
    const auto add = [&](const Att& a) {
        key.insert(key.end(), {uint64_t(a.format), uint64_t(a.load), uint64_t(a.store), uint64_t(a.stencil_load),
                               uint64_t(a.stencil_store), uint64_t(a.layout)});
    };
    key.push_back(colors.size());
    for (const Att& a : colors) add(a);
    if (depth) add(*depth);
    VkRenderPass& rp = g.passes[key];
    if (rp) return rp;
    std::vector<VkAttachmentDescription> desc;
    std::vector<VkAttachmentReference> refs;
    const auto describe = [&](const Att& a) {
        VkAttachmentDescription d{};
        d.format = a.format;
        d.samples = VK_SAMPLE_COUNT_1_BIT;
        d.loadOp = a.load;
        d.storeOp = a.store;
        d.stencilLoadOp = a.stencil_load;
        d.stencilStoreOp = a.stencil_store;
        d.initialLayout = d.finalLayout = a.layout;
        desc.push_back(d);
        return VkAttachmentReference{static_cast<uint32_t>(desc.size() - 1), a.layout};
    };
    for (const Att& a : colors)
        refs.push_back(a.format == VK_FORMAT_UNDEFINED ? VkAttachmentReference{VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED}
                                                       : describe(a));
    VkAttachmentReference dref{};
    if (depth) dref = describe(*depth);
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = static_cast<uint32_t>(refs.size());
    sub.pColorAttachments = refs.data();
    sub.pDepthStencilAttachment = depth ? &dref : nullptr;
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = static_cast<uint32_t>(desc.size());
    ci.pAttachments = desc.data();
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    vkCreateRenderPass(g.device, &ci, nullptr, &rp);
    return rp;
}

// synchronization2 flags -> the original barrier's (devices without VK_KHR_synchronization2)
VkPipelineStageFlags stage1(VkPipelineStageFlags2 s, bool src) {
    if (!s) return src ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    VkPipelineStageFlags r = static_cast<VkPipelineStageFlags>(s & 0x7FFFFFFFull);
    if (s & (VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_RESOLVE_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT |
             VK_PIPELINE_STAGE_2_CLEAR_BIT))
        r |= VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (s & (VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT))
        r |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
    if (s & VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT)
        r |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT;
    return r;
}
VkAccessFlags access1(VkAccessFlags2 a) {
    VkAccessFlags r = static_cast<VkAccessFlags>(a & 0x7FFFFFFFull);
    if (a & (VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT)) r |= VK_ACCESS_SHADER_READ_BIT;
    if (a & VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT) r |= VK_ACCESS_SHADER_WRITE_BIT;
    return r;
}

}  // namespace

void init(VkDevice device, bool core13, bool dynamic_rendering) {
    g.device = device;
    g.dynamic = dynamic_rendering;
    if (dynamic_rendering) {
        g.begin = proc<PFN_vkCmdBeginRendering>(device, core13, "vkCmdBeginRendering", "vkCmdBeginRenderingKHR");
        g.end = proc<PFN_vkCmdEndRendering>(device, core13, "vkCmdEndRendering", "vkCmdEndRenderingKHR");
        g.dynamic = g.begin && g.end;
    }
    g.barrier2 = proc<PFN_vkCmdPipelineBarrier2>(device, core13, "vkCmdPipelineBarrier2", "vkCmdPipelineBarrier2KHR");
}

void shutdown() {
    std::lock_guard l(g.m);
    for (auto& [k, fb] : g.framebuffers) vkDestroyFramebuffer(g.device, fb, nullptr);
    for (auto& [k, rp] : g.passes) vkDestroyRenderPass(g.device, rp, nullptr);
    g.framebuffers.clear();
    g.passes.clear();
}

bool dynamic_rendering() { return g.dynamic; }

void cmd_begin_rendering(VkCommandBuffer cmd, const VkRenderingInfo& info, const VkFormat* color_formats,
                         VkFormat depth_format) {
    if (g.dynamic) {
        g.begin(cmd, &info);
        return;
    }
    std::vector<Att> colors;
    std::vector<VkImageView> views;
    std::vector<VkClearValue> clears;
    for (uint32_t i = 0; i < info.colorAttachmentCount; ++i) {
        const VkRenderingAttachmentInfo& a = info.pColorAttachments[i];
        if (!a.imageView) {
            colors.push_back({VK_FORMAT_UNDEFINED});
            continue;
        }
        colors.push_back({color_formats[i], a.loadOp, VK_ATTACHMENT_LOAD_OP_DONT_CARE, a.storeOp,
                          VK_ATTACHMENT_STORE_OP_DONT_CARE, a.imageLayout});
        views.push_back(a.imageView);
        clears.push_back(a.clearValue);
    }
    // depth and stencil share one attachment; an aspect without its own VkRenderingAttachmentInfo keeps its contents
    const VkRenderingAttachmentInfo* d = info.pDepthAttachment && info.pDepthAttachment->imageView ? info.pDepthAttachment : nullptr;
    const VkRenderingAttachmentInfo* s = info.pStencilAttachment && info.pStencilAttachment->imageView ? info.pStencilAttachment : nullptr;
    Att depth{};
    if (d || s) {
        const VkRenderingAttachmentInfo& any = d ? *d : *s;
        depth = {depth_format,
                 d ? d->loadOp : VK_ATTACHMENT_LOAD_OP_LOAD,
                 s ? s->loadOp : VK_ATTACHMENT_LOAD_OP_LOAD,
                 d ? d->storeOp : VK_ATTACHMENT_STORE_OP_STORE,
                 s ? s->storeOp : VK_ATTACHMENT_STORE_OP_STORE,
                 any.imageLayout};
        views.push_back(any.imageView);
        VkClearValue c{};
        c.depthStencil = {d ? d->clearValue.depthStencil.depth : 0.0f, s ? s->clearValue.depthStencil.stencil : 0u};
        clears.push_back(c);
    }
    const uint32_t width = static_cast<uint32_t>(info.renderArea.offset.x) + info.renderArea.extent.width;
    const uint32_t height = static_cast<uint32_t>(info.renderArea.offset.y) + info.renderArea.extent.height;
    VkRenderPass rp;
    VkFramebuffer fb;
    {
        std::lock_guard l(g.m);
        rp = render_pass(colors, d || s ? &depth : nullptr);
        std::vector<uint64_t> key{reinterpret_cast<uint64_t>(rp), width, height, info.layerCount};
        for (VkImageView v : views) key.push_back(reinterpret_cast<uint64_t>(v));
        VkFramebuffer& f = g.framebuffers[key];
        if (!f) {
            VkFramebufferCreateInfo ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            ci.renderPass = rp;
            ci.attachmentCount = static_cast<uint32_t>(views.size());
            ci.pAttachments = views.data();
            ci.width = width;
            ci.height = height;
            ci.layers = std::max(info.layerCount, 1u);
            vkCreateFramebuffer(g.device, &ci, nullptr, &f);
        }
        fb = f;
    }
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = rp;
    bi.framebuffer = fb;
    bi.renderArea = info.renderArea;
    bi.clearValueCount = static_cast<uint32_t>(clears.size());
    bi.pClearValues = clears.data();
    vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
}

void cmd_end_rendering(VkCommandBuffer cmd) {
    if (g.dynamic) g.end(cmd);
    else vkCmdEndRenderPass(cmd);
}

void cmd_pipeline_barrier2(VkCommandBuffer cmd, const VkDependencyInfo& info) {
    if (g.barrier2) {
        g.barrier2(cmd, &info);
        return;
    }
    VkPipelineStageFlags src = 0, dst = 0;
    std::vector<VkMemoryBarrier> mem;
    std::vector<VkBufferMemoryBarrier> buf;
    std::vector<VkImageMemoryBarrier> img;
    for (uint32_t i = 0; i < info.memoryBarrierCount; ++i) {
        const VkMemoryBarrier2& b = info.pMemoryBarriers[i];
        src |= stage1(b.srcStageMask, true);
        dst |= stage1(b.dstStageMask, false);
        mem.push_back({VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, access1(b.srcAccessMask), access1(b.dstAccessMask)});
    }
    for (uint32_t i = 0; i < info.bufferMemoryBarrierCount; ++i) {
        const VkBufferMemoryBarrier2& b = info.pBufferMemoryBarriers[i];
        src |= stage1(b.srcStageMask, true);
        dst |= stage1(b.dstStageMask, false);
        buf.push_back({VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, nullptr, access1(b.srcAccessMask), access1(b.dstAccessMask),
                       b.srcQueueFamilyIndex, b.dstQueueFamilyIndex, b.buffer, b.offset, b.size});
    }
    for (uint32_t i = 0; i < info.imageMemoryBarrierCount; ++i) {
        const VkImageMemoryBarrier2& b = info.pImageMemoryBarriers[i];
        src |= stage1(b.srcStageMask, true);
        dst |= stage1(b.dstStageMask, false);
        img.push_back({VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, access1(b.srcAccessMask), access1(b.dstAccessMask),
                       b.oldLayout, b.newLayout, b.srcQueueFamilyIndex, b.dstQueueFamilyIndex, b.image, b.subresourceRange});
    }
    vkCmdPipelineBarrier(cmd, src ? src : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, dst ? dst : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         info.dependencyFlags, static_cast<uint32_t>(mem.size()), mem.data(),
                         static_cast<uint32_t>(buf.size()), buf.data(), static_cast<uint32_t>(img.size()), img.data());
}

VkRenderPass render_pass_for(const VkPipelineRenderingCreateInfo& rendering) {
    if (g.dynamic) return VK_NULL_HANDLE;
    std::vector<Att> colors;
    for (uint32_t i = 0; i < rendering.colorAttachmentCount; ++i)
        colors.push_back({rendering.pColorAttachmentFormats[i], VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                          VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
    const VkFormat df = rendering.depthAttachmentFormat ? rendering.depthAttachmentFormat : rendering.stencilAttachmentFormat;
    const Att depth{df, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
                    VK_ATTACHMENT_STORE_OP_STORE, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    std::lock_guard l(g.m);
    return render_pass(colors, df ? &depth : nullptr);
}

void pipeline_rendering(VkGraphicsPipelineCreateInfo& pci, const VkPipelineRenderingCreateInfo& rendering) {
    if (g.dynamic) return;
    pci.renderPass = render_pass_for(rendering);
    pci.subpass = 0;
    if (pci.pNext == &rendering) pci.pNext = rendering.pNext;
}

void forget_view(VkImageView view) {
    if (g.dynamic || !view) return;
    std::lock_guard l(g.m);
    for (auto it = g.framebuffers.begin(); it != g.framebuffers.end();) {
        if (std::find(it->first.begin() + 4, it->first.end(), reinterpret_cast<uint64_t>(view)) != it->first.end()) {
            vkDestroyFramebuffer(g.device, it->second, nullptr);
            it = g.framebuffers.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace mh3u::gfx::vkc
