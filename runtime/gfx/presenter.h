// Window + Vulkan presentation: an SDL3 window, Vulkan instance/device/swapchain, and a frame present path.
// Created and pumped on the host main thread (SDL requirement); present() may be called from any thread.
// A frame is a clear colour or an RGBA8 image (the TV scan buffer) blitted to fit the window.
// Headless mode creates the same Vulkan device without a window or swapchain (presents are no-ops), so rendering
// still runs for frame dumps and tests.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace mh3u::gfx {

// The device shared with the renderer. Every vkQueueSubmit on `queue` must hold `queue_mutex`.
struct VulkanContext {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    std::mutex* queue_mutex = nullptr;
    bool depth_clip_control = false;  // VK_EXT_depth_clip_control (GL -1..1 clip-space depth)
};

class Presenter {
public:
    Presenter();
    ~Presenter();
    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;

    // Creates the window and the Vulkan objects. Returns false (with a message in error()) on failure.
    bool init(const std::string& title, int width, int height, bool headless = false);
    // Processes window events; returns false once the user closed the window. Main thread only.
    bool pump_events();
    // Every SDL_Event (as const SDL_Event*) is also passed here, e.g. to host input.
    void set_event_hook(void (*hook)(const void* sdl_event)) { event_hook_ = hook; }
    void* window_handle() const;  // SDL_Window*, null when headless
    double refresh_rate() const;  // of the display the window is on (60 if unknown)
    // 0 windowed, 1 borderless (fills the display at its desktop resolution), 2 exclusive fullscreen. Main thread.
    void set_display_mode(int mode);
    // Windowed mode: the window's size (client area), centred on its display. Main thread.
    void set_window_size(int width, int height);
    // Presents one frame cleared to the given colour (thread-safe).
    void present_clear(float r, float g, float b);
    // Presents an RGBA8 image (row-major, width x height), scaled to fit the window (thread-safe).
    void present_image(const uint8_t* rgba, uint32_t width, uint32_t height);
    // Presents a device image (TRANSFER_SRC_OPTIMAL, written by an earlier submission on the queue), scaled to fit.
    void present_vk_image(VkImage image, uint32_t width, uint32_t height);
    // Drawn over every presented frame (the launcher): called with the queue lock held, inside dynamic rendering on
    // the swapchain image (colour attachment, contents kept), from whichever thread presents.
    using Overlay = std::function<void(VkCommandBuffer cmd, VkExtent2D extent)>;
    void set_overlay(Overlay overlay);
    VkFormat surface_format() const;  // of the swapchain images (the overlay's pipelines render to it)
    // Rebuilds the swapchain before the next present (after MH3U_VSYNC changed).
    void refresh_swapchain();
    // Whether an image (a game frame) was presented within the last `ms` milliseconds.
    bool showing_image(int ms) const;
    // Writes the next presented frame, as shown (overlay included), to a BMP file. For checking the UI.
    void capture(const std::string& path);

    const VulkanContext& vk() const;

    const std::string& error() const { return error_; }
    const std::string& device_name() const;
    uint64_t frames_presented() const;

private:
    void present(const uint8_t* rgba, VkImage image, uint32_t width, uint32_t height, float r, float g, float b);
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string error_;
    void (*event_hook_)(const void*) = nullptr;
};

}  // namespace mh3u::gfx
