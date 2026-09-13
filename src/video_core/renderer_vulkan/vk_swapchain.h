// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <mutex>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_vulkan/swapchain_acquire.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Frontend {
class Window;
}

namespace Vulkan {

class Instance;
class Scheduler;

class Swapchain {
public:
    explicit Swapchain(const Instance& instance, const Frontend::Window& window);
    ~Swapchain();

    /// Creates (or recreates) the swapchain with a given size.
    void Create(u32 width, u32 height);

    /// Recreates the swapchain with a given size and current surface.
    void Recreate(u32 width, u32 height);

    /// Acquires the next image in the swapchain.
    AcquireStatus AcquireNextImage();

    /// Presents the current image and move to the next one
    bool Present();
    u64 SuccessfulPresents() const { return successful_presents.load(); }
    std::atomic<u64> successful_presents{};

    /// Stops subsequent acquire calls. An already-running call has a finite
    /// timeout; an image it acquired successfully must still be consumed.
    /// The session owner calls this before joining rendering workers.
    void RequestStop() {
        stop_requested.store(true, std::memory_order_release);
    }

    bool StopRequested() const {
        return stop_requested.load(std::memory_order_acquire);
    }

    vk::SurfaceKHR GetSurface() const {
        return surface;
    }

    vk::Image Image() const {
        return images[image_index];
    }

    vk::ImageView ImageView() const {
        return images_view[image_index];
    }

    vk::SurfaceFormatKHR GetSurfaceFormat() const {
        return surface_format;
    }

    vk::SwapchainKHR GetHandle() const {
        return swapchain;
    }

    u32 GetWidth() const {
        return width;
    }

    u32 GetHeight() const {
        return height;
    }

    u32 GetImageCount() const {
        return image_count;
    }

    u32 GetFrameIndex() const {
        return frame_index;
    }

    vk::Extent2D GetExtent() const {
        return extent;
    }

    [[nodiscard]] vk::Semaphore GetImageAcquiredSemaphore() const {
        return image_acquired[frame_index];
    }

    [[nodiscard]] vk::Semaphore GetPresentReadySemaphore() const {
        return present_ready[image_index];
    }

    bool HasHDR() const {
        return supports_hdr;
    }

    void SetHDR(bool hdr);

    bool GetHDR() const {
        return needs_hdr;
    }

private:
    /// Selects the best available swapchain image format
    void FindPresentFormat();

    /// Selects the best available present mode
    void FindPresentMode();

    /// Sets the surface properties according to device capabilities
    void SetSurfaceProperties();

    /// Destroys the current VkSurfaceKHR and creates a fresh one from the window's
    /// current native handle. Needed on Android when the platform hands back a new
    /// ANativeWindow* (surface loss / backgrounding / rotation): the swapchain
    /// alone cannot recover, the surface object itself is dead. No-op-equivalent on
    /// desktop, which never loses its surface.
    void RebuildSurface();

    /// Destroys current swapchain resources
    void Destroy();

    /// Performs creation of image views and framebuffers from the swapchain images
    void SetupImages();

    /// Creates the image acquired and present ready semaphores
    void RefreshSemaphores();

private:
    const Instance& instance;
    const Frontend::Window& window;
    vk::SwapchainKHR swapchain{};
    vk::SurfaceKHR surface{};
    vk::SurfaceFormatKHR surface_format;
    vk::Format view_format;
    vk::PresentModeKHR present_mode;
    vk::Extent2D extent;
    vk::SurfaceTransformFlagBitsKHR transform;
    vk::CompositeAlphaFlagBitsKHR composite_alpha;
    std::vector<vk::Image> images;
    std::vector<vk::ImageView> images_view;
    std::vector<vk::Semaphore> image_acquired;
    std::vector<vk::Semaphore> present_ready;
    u32 width = 0;
    u32 height = 0;
    u32 image_count = 0;
    u32 image_index = 0;
    u32 frame_index = 0;
    bool needs_recreation = true;
    // Set when acquire/present returned eErrorSurfaceLostKHR: the next Recreate
    // must rebuild the VkSurfaceKHR, not just the swapchain.
    bool surface_lost = false;
    // Set by RequestStop() from any thread; prevents further acquires.
    std::atomic<bool> stop_requested{false};
    bool needs_hdr = false;    // The game requested HDR swapchain
    bool supports_hdr = false; // SC supports HDR output
};

} // namespace Vulkan
