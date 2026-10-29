#include "presenter.h"

#include "gx2.h"
#include "vk_compat.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <vector>

namespace mh3u::gfx {

struct Presenter::Impl {
    SDL_Window* window = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;  // per swapchain image, for the overlay
    Overlay overlay;
    std::atomic<int64_t> last_image_ms{-1000000};  // steady clock, when an image was last presented
    std::string capture_path;  // capture(): written after the next present
    std::vector<VkSemaphore> render_done;  // one per swapchain image
    VkCommandPool pool = VK_NULL_HANDLE;
    // presents in flight: present() waits only for the one kInFlight frames back, so the CPU does not wait for the GPU
    // to finish the previous frame
    static constexpr uint32_t kInFlight = 2;
    VkCommandBuffer cmd[kInFlight]{};
    VkSemaphore acquired[kInFlight]{};
    VkFence fence[kInFlight]{};
    uint32_t slot = 0;
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    std::string device_name;
    std::mutex m;
    VulkanContext ctx;
    std::atomic<uint64_t> frames{0};
    bool needs_recreate = false;
    // present_image: host staging buffer -> source image -> scaled blit into the swapchain image
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE, src_mem = VK_NULL_HANDLE;
    void* staging_map = nullptr;
    VkImage src_image = VK_NULL_HANDLE;
    uint32_t src_w = 0, src_h = 0;

    uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags props) const;
    bool ensure_source(uint32_t w, uint32_t h);
    void destroy_source();

    bool create_swapchain(std::string& err);
    void destroy_swapchain();
};

Presenter::Presenter() : impl_(std::make_unique<Impl>()) {}

Presenter::~Presenter() {
    Impl& d = *impl_;
    if (d.device) {
        vkDeviceWaitIdle(d.device);
        d.destroy_swapchain();
        d.destroy_source();
        for (uint32_t i = 0; i < Impl::kInFlight; ++i) {
            if (d.fence[i]) vkDestroyFence(d.device, d.fence[i], nullptr);
            if (d.acquired[i]) vkDestroySemaphore(d.device, d.acquired[i], nullptr);
        }
        if (d.pool) vkDestroyCommandPool(d.device, d.pool, nullptr);
        vkc::shutdown();
        vkDestroyDevice(d.device, nullptr);
    }
    if (d.surface) vkDestroySurfaceKHR(d.instance, d.surface, nullptr);
    if (d.instance) vkDestroyInstance(d.instance, nullptr);
    if (d.window) SDL_DestroyWindow(d.window);
    SDL_Quit();
}

const std::string& Presenter::device_name() const { return impl_->device_name; }
const VulkanContext& Presenter::vk() const { return impl_->ctx; }
void* Presenter::window_handle() const { return impl_->window; }
double Presenter::refresh_rate() const {
    const SDL_DisplayMode* m = impl_->window ? SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(impl_->window)) : nullptr;
    return m && m->refresh_rate > 1.0f ? static_cast<double>(m->refresh_rate) : 60.0;
}
uint64_t Presenter::frames_presented() const { return impl_->frames.load(); }

void Presenter::set_display_mode(int mode) {
    SDL_Window* w = impl_->window;
    if (!w) return;
    if (mode == 0) {
        SDL_SetWindowFullscreen(w, false);
        return;
    }
    // borderless: SDL's desktop fullscreen (null mode); exclusive: the display's own desktop mode
    const SDL_DisplayMode* m = mode == 2 ? SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(w)) : nullptr;
    SDL_SetWindowFullscreenMode(w, m);
    SDL_SetWindowFullscreen(w, true);
}

void Presenter::set_window_size(int width, int height) {
    SDL_Window* w = impl_->window;
    if (!w || (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN)) return;
    SDL_SetWindowSize(w, width, height);
    SDL_SetWindowPosition(w, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

void Presenter::Impl::destroy_swapchain() {
    for (VkSemaphore s : render_done) vkDestroySemaphore(device, s, nullptr);
    render_done.clear();
    for (VkImageView v : views) {
        vkc::forget_view(v);
        vkDestroyImageView(device, v, nullptr);
    }
    views.clear();
    images.clear();
    if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
    swapchain = VK_NULL_HANDLE;
}

bool Presenter::Impl::create_swapchain(std::string& err) {
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps);
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &n, formats.data());
    VkSurfaceFormatKHR chosen = formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : formats[0];
    for (auto& f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM) chosen = f;
    format = chosen.format;
    if (caps.currentExtent.width != UINT32_MAX) {
        extent = caps.currentExtent;
    } else {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
    }
    if (extent.width == 0 || extent.height == 0) return true;  // minimised: try again later
    uint32_t count = std::max(caps.minImageCount + 1, 2u);
    if (caps.maxImageCount) count = std::min(count, caps.maxImageCount);
    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface;
    sci.minImageCount = count;
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                     (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);  // capture()
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    // The game paces itself (GX2SwapScanBuffers sleeps to its flip time), so presenting must not block on the
    // display's vsync as well: two unsynchronised clocks stacked make every late frame wait for both. MAILBOX (no
    // tearing, never blocks) when available, else IMMEDIATE; FIFO only if neither exists or MH3U_VSYNC=1 asks for it.
    std::vector<VkPresentModeKHR> modes;
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &n, nullptr);
    modes.resize(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &n, modes.data());
    const auto has = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
    char* vsync = nullptr;
    size_t vlen = 0;
    const bool force_fifo = _dupenv_s(&vsync, &vlen, "MH3U_VSYNC") == 0 && vsync && *vsync == '1';
    std::free(vsync);
    present_mode = force_fifo                             ? VK_PRESENT_MODE_FIFO_KHR
                   : has(VK_PRESENT_MODE_MAILBOX_KHR)   ? VK_PRESENT_MODE_MAILBOX_KHR
                   : has(VK_PRESENT_MODE_IMMEDIATE_KHR) ? VK_PRESENT_MODE_IMMEDIATE_KHR
                                                        : VK_PRESENT_MODE_FIFO_KHR;
    sci.presentMode = present_mode;
    std::printf("swapchain: %ux%u, %s\n", extent.width, extent.height,
                present_mode == VK_PRESENT_MODE_MAILBOX_KHR     ? "mailbox"
                : present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate"
                                                                : "fifo (vsync)");
    sci.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(device, &sci, nullptr, &swapchain) != VK_SUCCESS) {
        err = "vkCreateSwapchainKHR failed";
        return false;
    }
    vkGetSwapchainImagesKHR(device, swapchain, &n, nullptr);
    images.resize(n);
    vkGetSwapchainImagesKHR(device, swapchain, &n, images.data());
    render_done.resize(n);
    views.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = images[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(device, &vci, nullptr, &views[i]);
    }
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (auto& s : render_done) vkCreateSemaphore(device, &si, nullptr, &s);
    return true;
}

bool Presenter::init(const std::string& title, int width, int height, bool headless) {
    Impl& d = *impl_;
    auto fail = [&](const std::string& why) {
        error_ = why;
        return false;
    };
    Uint32 n_ext = 0;
    const char* const* ext = nullptr;
    if (!headless) {
        // window/taskbar icon: the executable's icon resource 1 (runtime/boot/mh3u.rc)
        SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
        SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
        if (!SDL_Init(SDL_INIT_VIDEO)) return fail(std::string("SDL_Init: ") + SDL_GetError());
        d.window = SDL_CreateWindow(title.c_str(), width, height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
        if (!d.window) return fail(std::string("SDL_CreateWindow: ") + SDL_GetError());
        SDL_SetWindowMinimumSize(d.window, 960, 540);  // the launcher's layout needs at least this much
        ext = SDL_Vulkan_GetInstanceExtensions(&n_ext);
        if (!ext) return fail(std::string("SDL_Vulkan_GetInstanceExtensions: ") + SDL_GetError());
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Cha-Cha";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = n_ext;
    ici.ppEnabledExtensionNames = ext;
    const char* validation = "VK_LAYER_KHRONOS_validation";
    char* want_validation = nullptr;
    size_t len = 0;
    if (_dupenv_s(&want_validation, &len, "MH3U_VK_VALIDATION") == 0 && want_validation) {
        ici.enabledLayerCount = 1;
        ici.ppEnabledLayerNames = &validation;
    }
    std::free(want_validation);
    if (vkCreateInstance(&ici, nullptr, &d.instance) != VK_SUCCESS) return fail("vkCreateInstance failed");
    if (!headless && !SDL_Vulkan_CreateSurface(d.window, d.instance, nullptr, &d.surface))
        return fail(std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError());

    // physical device: one with a queue family that does graphics and presents to the window; prefer discrete
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(d.instance, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(d.instance, &n, devs.data());
    int best_score = -1;
    for (VkPhysicalDevice pd : devs) {
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qf(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf.data());
        for (uint32_t i = 0; i < qn; ++i) {
            VkBool32 present = headless ? VK_TRUE : VK_FALSE;
            if (!headless) vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, d.surface, &present);
            if (!(qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(pd, &props);
            int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (score > best_score) {
                best_score = score;
                d.phys = pd;
                d.queue_family = i;
                d.device_name = props.deviceName;
            }
            break;
        }
    }
    if (!d.phys) return fail("no Vulkan device can present to the window");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = d.queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    // Extensions/features the renderer and the translated shaders need (docs/runtime.md § Rendering, stage 2).
    uint32_t n_avail = 0;
    vkEnumerateDeviceExtensionProperties(d.phys, nullptr, &n_avail, nullptr);
    std::vector<VkExtensionProperties> avail(n_avail);
    vkEnumerateDeviceExtensionProperties(d.phys, nullptr, &n_avail, avail.data());
    const auto has = [&](const char* name) {
        return std::any_of(avail.begin(), avail.end(), [&](const VkExtensionProperties& e) { return !std::strcmp(e.extensionName, name); });
    };
    std::vector<const char*> dev_ext;
    if (!headless) dev_ext.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    // Vulkan 1.3 has dynamic rendering, synchronization2 and shader demote in core. Older drivers (Vulkan 1.1 / 1.2,
    // e.g. AMD's last Windows driver for GCN 1 Radeons) get them from extensions; dynamic rendering and
    // synchronization2 are emulated where missing (vk_compat.h). Shader demote is required: the translated pixel
    // shaders' alpha test and kill use it. MH3U_VK_LEGACY=1 takes the old-driver path on a new GPU (no dynamic
    // rendering, no synchronization2) for testing.
    char* legacy_env = nullptr;
    size_t legacy_len = 0;
    const bool legacy = _dupenv_s(&legacy_env, &legacy_len, "MH3U_VK_LEGACY") == 0 && legacy_env;
    std::free(legacy_env);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(d.phys, &props);
    d.ctx.api_version = props.apiVersion;
    if (props.apiVersion < VK_API_VERSION_1_1) return fail("device supports only Vulkan 1.0 (Cha-Cha needs 1.1 or newer)");
    const bool core13 = props.apiVersion >= VK_API_VERSION_1_3 && !legacy;
    void* chain = nullptr;
    const auto link = [&](auto& feature) {
        feature.pNext = chain;
        chain = &feature;
    };
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceDynamicRenderingFeaturesKHR dyn{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
    VkPhysicalDeviceSynchronization2FeaturesKHR sync2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR};
    VkPhysicalDeviceShaderDemoteToHelperInvocationFeaturesEXT demote{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES_EXT};
    bool dynamic_rendering = true;
    if (core13) {
        f13.dynamicRendering = VK_TRUE;
        f13.shaderDemoteToHelperInvocation = VK_TRUE;
        f13.synchronization2 = VK_TRUE;
        link(f13);
    } else {
        if (has(VK_EXT_SHADER_DEMOTE_TO_HELPER_INVOCATION_EXTENSION_NAME)) {
            dev_ext.push_back(VK_EXT_SHADER_DEMOTE_TO_HELPER_INVOCATION_EXTENSION_NAME);
            demote.shaderDemoteToHelperInvocation = VK_TRUE;
            link(demote);
        } else if (props.apiVersion >= VK_API_VERSION_1_3) {  // MH3U_VK_LEGACY on a driver without the extension
            f13.shaderDemoteToHelperInvocation = VK_TRUE;
            link(f13);
        } else {
            return fail("device lacks VK_EXT_shader_demote_to_helper_invocation");
        }
        dynamic_rendering = !legacy && has(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
        if (dynamic_rendering) {
            if (props.apiVersion < VK_API_VERSION_1_2) {  // its dependencies are core in 1.2
                for (const char* e : {VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME, VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME})
                    dynamic_rendering = dynamic_rendering && has(e);
                if (dynamic_rendering)
                    dev_ext.insert(dev_ext.end(), {VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME, VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME});
            }
            if (dynamic_rendering) {
                dev_ext.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
                dyn.dynamicRendering = VK_TRUE;
                link(dyn);
            }
        }
        if (!legacy && has(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)) {
            dev_ext.push_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
            sync2.synchronization2 = VK_TRUE;
            link(sync2);
        }
    }
    VkPhysicalDeviceDepthClipControlFeaturesEXT clip{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
    if (has(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME)) {
        dev_ext.push_back(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME);
        clip.depthClipControl = VK_TRUE;
        link(clip);
        d.ctx.depth_clip_control = true;
    }
    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(d.phys, &supported);
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = chain;
    f2.features.geometryShader = supported.geometryShader;
    f2.features.largePoints = supported.largePoints;
    f2.features.independentBlend = supported.independentBlend;
    f2.features.dualSrcBlend = supported.dualSrcBlend;
    f2.features.logicOp = supported.logicOp;
    f2.features.fillModeNonSolid = supported.fillModeNonSolid;
    f2.features.depthClamp = supported.depthClamp;
    f2.features.samplerAnisotropy = supported.samplerAnisotropy;
    f2.features.textureCompressionBC = supported.textureCompressionBC;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(dev_ext.size());
    dci.ppEnabledExtensionNames = dev_ext.data();
    if (vkCreateDevice(d.phys, &dci, nullptr, &d.device) != VK_SUCCESS) return fail("vkCreateDevice failed");
    vkc::init(d.device, core13, dynamic_rendering);
    if (!core13)
        std::fprintf(stderr, "Vulkan %u.%u device%s: %s, %s\n", VK_API_VERSION_MAJOR(props.apiVersion),
                     VK_API_VERSION_MINOR(props.apiVersion), legacy ? " (MH3U_VK_LEGACY)" : "",
                     dynamic_rendering ? "dynamic rendering" : "render passes",
                     sync2.synchronization2 ? "synchronization2" : "classic barriers");
    vkGetDeviceQueue(d.device, d.queue_family, 0, &d.queue);
    d.ctx.instance = d.instance;
    d.ctx.phys = d.phys;
    d.ctx.device = d.device;
    d.ctx.queue = d.queue;
    d.ctx.queue_family = d.queue_family;
    d.ctx.queue_mutex = &d.m;

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = d.queue_family;
    vkCreateCommandPool(d.device, &pci, nullptr, &d.pool);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = d.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = Impl::kInFlight;
    vkAllocateCommandBuffers(d.device, &cai, d.cmd);
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (uint32_t i = 0; i < Impl::kInFlight; ++i) {
        vkCreateSemaphore(d.device, &si, nullptr, &d.acquired[i]);
        vkCreateFence(d.device, &fi, nullptr, &d.fence[i]);
    }

    std::string err;
    if (!headless && !d.create_swapchain(err)) return fail(err);
    return true;
}

bool Presenter::pump_events() {
    if (!impl_->window) return true;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (event_hook_) event_hook_(&e);
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F3) cycle_gamepad_view();
        if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) return false;
        if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
            std::lock_guard g(impl_->m);
            impl_->needs_recreate = true;
        }
    }
    return true;
}

namespace {
int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

void Presenter::set_overlay(Overlay overlay) {
    std::lock_guard g(impl_->m);
    impl_->overlay = std::move(overlay);
}
VkFormat Presenter::surface_format() const { return impl_->format; }
void Presenter::refresh_swapchain() {
    std::lock_guard g(impl_->m);
    impl_->needs_recreate = true;
}
bool Presenter::showing_image(int ms) const { return now_ms() - impl_->last_image_ms.load() < ms; }

void Presenter::present_clear(float r, float g, float b) { present(nullptr, VK_NULL_HANDLE, 0, 0, r, g, b); }

void Presenter::present_image(const uint8_t* rgba, uint32_t width, uint32_t height) {
    present(rgba, VK_NULL_HANDLE, width, height, 0.0f, 0.0f, 0.0f);
}

void Presenter::present_vk_image(VkImage image, uint32_t width, uint32_t height) {
    present(nullptr, image, width, height, 0.0f, 0.0f, 0.0f);
}

uint32_t Presenter::Impl::memory_type(uint32_t bits, VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    return UINT32_MAX;
}

void Presenter::Impl::destroy_source() {
    if (staging_map) vkUnmapMemory(device, staging_mem);
    if (staging) vkDestroyBuffer(device, staging, nullptr);
    if (staging_mem) vkFreeMemory(device, staging_mem, nullptr);
    if (src_image) vkDestroyImage(device, src_image, nullptr);
    if (src_mem) vkFreeMemory(device, src_mem, nullptr);
    staging = VK_NULL_HANDLE;
    staging_mem = src_mem = VK_NULL_HANDLE;
    src_image = VK_NULL_HANDLE;
    staging_map = nullptr;
    src_w = src_h = 0;
}

bool Presenter::Impl::ensure_source(uint32_t w, uint32_t h) {
    if (src_image && src_w == w && src_h == h) return true;
    vkDeviceWaitIdle(device);
    destroy_source();
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = VkDeviceSize{w} * h * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(device, &bci, nullptr, &staging) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, staging, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex =
        memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device, &mai, nullptr, &staging_mem) != VK_SUCCESS)
        return false;
    vkBindBufferMemory(device, staging, staging_mem, 0);
    vkMapMemory(device, staging_mem, 0, VK_WHOLE_SIZE, 0, &staging_map);

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &ici, nullptr, &src_image) != VK_SUCCESS) return false;
    vkGetImageMemoryRequirements(device, src_image, &req);
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device, &mai, nullptr, &src_mem) != VK_SUCCESS)
        return false;
    vkBindImageMemory(device, src_image, src_mem, 0);
    src_w = w;
    src_h = h;
    return true;
}

namespace {
void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
             VkAccessFlags dst_access, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}
}  // namespace

// One frame: acquire; clear to (r, g, b); if an image is given (CPU RGBA8 or a device image), blit it scaled to
// fit; present.
void Presenter::present(const uint8_t* rgba, VkImage image, uint32_t width, uint32_t height, float r, float g, float b) {
    Impl& d = *impl_;
    std::lock_guard lock(d.m);
    if (!d.device || !d.surface) return;
    const uint32_t s = d.slot;
    // the CPU-pixels path reuses one staging image: every present that may still read it must be done
    if (rgba) vkWaitForFences(d.device, Impl::kInFlight, d.fence, VK_TRUE, UINT64_MAX);
    else vkWaitForFences(d.device, 1, &d.fence[s], VK_TRUE, UINT64_MAX);
    if (d.needs_recreate || !d.swapchain) {
        vkDeviceWaitIdle(d.device);
        d.destroy_swapchain();
        std::string err;
        d.create_swapchain(err);
        d.needs_recreate = false;
        if (!d.swapchain) return;
    }
    if (rgba && (width == 0 || height == 0 || !d.ensure_source(width, height))) rgba = nullptr;
    uint32_t index = 0;
    VkResult res = vkAcquireNextImageKHR(d.device, d.swapchain, UINT64_MAX, d.acquired[s], VK_NULL_HANDLE, &index);
    if (res == VK_ERROR_OUT_OF_DATE_KHR) {
        d.needs_recreate = true;
        return;
    }
    VkCommandBuffer cmd = d.cmd[s];
    vkResetFences(d.device, 1, &d.fence[s]);
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const VkImage target = d.images[index];
    barrier(cmd, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue color{};
    color.float32[0] = r;
    color.float32[1] = g;
    color.float32[2] = b;
    color.float32[3] = 1.0f;
    vkCmdClearColorImage(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
    VkImage source = VK_NULL_HANDLE;
    if (image && width && height) {
        // written by an earlier submission: make those transfer writes visible to the blit
        barrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        source = image;
    }
    if (rgba) {
        source = d.src_image;
        std::memcpy(d.staging_map, rgba, size_t{width} * height * 4);
        barrier(cmd, d.src_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(cmd, d.staging, d.src_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier(cmd, d.src_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
    }
    if (source) {
        d.last_image_ms = now_ms();
        // keep the aspect ratio: fit inside the window, centred
        const double scale = std::min(static_cast<double>(d.extent.width) / width,
                                      static_cast<double>(d.extent.height) / height);
        const int32_t w = static_cast<int32_t>(width * scale), h = static_cast<int32_t>(height * scale);
        const int32_t x0 = (static_cast<int32_t>(d.extent.width) - w) / 2;
        const int32_t y0 = (static_cast<int32_t>(d.extent.height) - h) / 2;
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {static_cast<int32_t>(width), static_cast<int32_t>(height), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0] = {x0, y0, 0};
        blit.dstOffsets[1] = {x0 + w, y0 + h, 1};
        vkCmdBlitImage(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }
    VkImageLayout layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    if (d.overlay) {
        barrier(cmd, target, layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        ca.imageView = d.views[index];
        ca.imageLayout = layout;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, d.extent};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
        vkc::cmd_begin_rendering(cmd, ri, &d.format);
        d.overlay(cmd, d.extent);
        vkc::cmd_end_rendering(cmd);
    }
    const bool drew = layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    // capture(): the finished frame (overlay included) into a host buffer
    VkBuffer cap_buf = VK_NULL_HANDLE;
    VkDeviceMemory cap_mem = VK_NULL_HANDLE;
    const std::string cap_path = std::move(d.capture_path);
    d.capture_path.clear();
    if (!cap_path.empty()) {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = VkDeviceSize{d.extent.width} * d.extent.height * 4;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        vkCreateBuffer(d.device, &bci, nullptr, &cap_buf);
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(d.device, cap_buf, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = d.memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(d.device, &mai, nullptr, &cap_mem);
        vkBindBufferMemory(d.device, cap_buf, cap_mem, 0);
        barrier(cmd, target, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {d.extent.width, d.extent.height, 1};
        vkCmdCopyImageToBuffer(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, cap_buf, 1, &copy);
        layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }
    barrier(cmd, target, layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            drew ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_TRANSFER_WRITE_BIT, 0,
            drew ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &d.acquired[s];
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &d.render_done[index];
    vkQueueSubmit(d.queue, 1, &submit, d.fence[s]);
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &d.render_done[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &d.swapchain;
    pi.pImageIndices = &index;
    res = vkQueuePresentKHR(d.queue, &pi);
    if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) d.needs_recreate = true;
    d.slot = (s + 1) % Impl::kInFlight;
    ++d.frames;
    if (cap_buf) {  // a 32-bit BMP (the swapchain is BGRA: the bytes go straight in, rows bottom-up)
        vkWaitForFences(d.device, 1, &d.fence[s], VK_TRUE, UINT64_MAX);
        void* map = nullptr;
        vkMapMemory(d.device, cap_mem, 0, VK_WHOLE_SIZE, 0, &map);
        const uint32_t w = d.extent.width, h = d.extent.height;
        std::FILE* f = nullptr;
        if (fopen_s(&f, cap_path.c_str(), "wb") == 0 && f) {
            uint8_t hdr[54] = {'B', 'M'};
            const uint32_t size = 54 + w * h * 4;
            std::memcpy(hdr + 2, &size, 4);
            hdr[10] = 54;
            hdr[14] = 40;
            std::memcpy(hdr + 18, &w, 4);
            std::memcpy(hdr + 22, &h, 4);
            hdr[26] = 1;
            hdr[28] = 32;
            std::fwrite(hdr, 1, 54, f);
            for (uint32_t y = h; y-- > 0;) std::fwrite(static_cast<uint8_t*>(map) + size_t{y} * w * 4, 1, size_t{w} * 4, f);
            std::fclose(f);
            std::printf("capture: %s (%ux%u)\n", cap_path.c_str(), w, h);
        }
        vkUnmapMemory(d.device, cap_mem);
        vkDestroyBuffer(d.device, cap_buf, nullptr);
        vkFreeMemory(d.device, cap_mem, nullptr);
    }
}

void Presenter::capture(const std::string& path) {
    std::lock_guard g(impl_->m);
    impl_->capture_path = path;
}

}  // namespace mh3u::gfx
