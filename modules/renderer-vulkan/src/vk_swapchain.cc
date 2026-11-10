// Korvid renderer-vulkan: the swapchain and the frames in flight. Recreation is fenced:
// the new chain is built with the old as oldSwapchain, and the old chain, its views and
// its per-image semaphores retire through the garbage queue of the frame being recorded,
// whose fence passes after every earlier frame on the queue. Nothing waits for the device
// to idle except renderer_destroy.
#include "renderer_internal.h"

namespace kv {

namespace {

// Queues the current chain's objects for destruction after the frame and forgets them.
void retire_swapchain(Renderer* r) {
    Swapchain& s = r->swapchain;
    for (VkSemaphore sem : s.present) rv_defer_destroy(r, Garbage::SemaphoreKind, (u64)sem);
    for (VkImageView v : s.views) rv_defer_destroy(r, Garbage::ImageViewKind, (u64)v);
    rv_defer_destroy(r, Garbage::SwapchainKind, (u64)s.handle);
    s.present.clear();
    s.views.clear();
    s.images.clear();
    s.handle = VK_NULL_HANDLE;
    s.extent = {0, 0};
}

// Builds the swapchain for the current surface size. Returns true with a zero extent when
// the window has no area (minimized); the caller then skips the frame.
bool create_swapchain(Renderer* r) {
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r->gpu, r->surface, &caps));

    // Wayland leaves currentExtent undefined; the window's pixel size decides then.
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width  = r->want_width;
        extent.height = r->want_height;
    }
    extent.width  = extent.width  < caps.minImageExtent.width  ? caps.minImageExtent.width  :
                    extent.width  > caps.maxImageExtent.width  ? caps.maxImageExtent.width  : extent.width;
    extent.height = extent.height < caps.minImageExtent.height ? caps.minImageExtent.height :
                    extent.height > caps.maxImageExtent.height ? caps.maxImageExtent.height : extent.height;
    if (extent.width == 0 || extent.height == 0) {
        if (r->swapchain.handle) retire_swapchain(r);
        return true;
    }

    u32 fmt_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(r->gpu, r->surface, &fmt_count, nullptr);
    Array<VkSurfaceFormatKHR> formats;
    formats.push_n(fmt_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(r->gpu, r->surface, &fmt_count, formats.data);
    VkSurfaceFormatKHR chosen = formats[0];
    for (u32 i = 0; i < fmt_count; ++i) {
        if ((formats[i].format == VK_FORMAT_B8G8R8A8_SRGB || formats[i].format == VK_FORMAT_R8G8B8A8_SRGB) &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = formats[i]; break; }
    }
    formats.free();

    u32 mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(r->gpu, r->surface, &mode_count, nullptr);
    Array<VkPresentModeKHR> modes;
    modes.push_n(mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(r->gpu, r->surface, &mode_count, modes.data);
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR; // always available
    if (!r->vsync) {
        for (u32 i = 0; i < mode_count; ++i)
            if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) present_mode = modes[i];
        if (present_mode == VK_PRESENT_MODE_FIFO_KHR)
            for (u32 i = 0; i < mode_count; ++i)
                if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) present_mode = modes[i];
    }
    modes.free();

    u32 image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) image_count = caps.maxImageCount;

    VkSwapchainKHR old = r->swapchain.handle;
    VkSwapchainCreateInfoKHR ci{};
    ci.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface          = r->surface;
    ci.minImageCount    = image_count;
    ci.imageFormat      = chosen.format;
    ci.imageColorSpace  = chosen.colorSpace;
    ci.imageExtent      = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT; // screenshots
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform     = caps.currentTransform;
    ci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode      = present_mode;
    ci.clipped          = VK_TRUE;
    ci.oldSwapchain     = old;

    VkSwapchainKHR sc = VK_NULL_HANDLE;
    VkResult res = vkCreateSwapchainKHR(r->device, &ci, nullptr, &sc);
    if (old) retire_swapchain(r); // the old chain may still be presenting: it goes with the frame
    if (res != VK_SUCCESS) {
        KV_ERROR("vulkan", "vkCreateSwapchainKHR: %s", vk_result_name(res));
        return false;
    }
    Swapchain& s = r->swapchain;
    s.handle = sc;
    s.format = chosen.format;
    s.extent = extent;

    u32 n = 0;
    vkGetSwapchainImagesKHR(r->device, sc, &n, nullptr);
    s.images.push_n(n);
    vkGetSwapchainImagesKHR(r->device, sc, &n, s.images.data);
    for (u32 i = 0; i < n; ++i) {
        VkImageViewCreateInfo vci{};
        vci.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image            = s.images[i];
        vci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        vci.format           = s.format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view;
        VK_CHECK(vkCreateImageView(r->device, &vci, nullptr, &view));
        s.views.push(view);

        VkSemaphoreCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkSemaphore sem;
        VK_CHECK(vkCreateSemaphore(r->device, &sci, nullptr, &sem));
        s.present.push(sem);
    }
    // A live resize rebuilds this every frame; only the first one is worth an INFO line.
    log_write(r->swapchain_generation++ == 0 ? LogLevel::Info : LogLevel::Debug, "vulkan",
              "swapchain %ux%u, %u images, format %d, %s", extent.width, extent.height, n,
              (int)s.format, present_mode == VK_PRESENT_MODE_FIFO_KHR ? "fifo" :
              present_mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "immediate");
    return true;
}

} // namespace

bool rv_recreate_swapchain(Renderer* r) {
    VkFormat before = r->swapchain.format;
    if (!create_swapchain(r)) return false;
    r->swapchain_dirty = false;
    if (r->swapchain.handle && (before != r->swapchain.format || !r->pipelines[0])) {
        rv_defer_destroy_pipelines(r);
        if (!rv_create_pipelines(r)) return false;
    }
    return true;
}

// Device idle only.
void rv_destroy_swapchain_now(Renderer* r) {
    Swapchain& s = r->swapchain;
    for (VkSemaphore sem : s.present) vkDestroySemaphore(r->device, sem, nullptr);
    for (VkImageView v : s.views) vkDestroyImageView(r->device, v, nullptr);
    if (s.handle) vkDestroySwapchainKHR(r->device, s.handle, nullptr);
    s.present.free();
    s.views.free();
    s.images.free();
    s.handle = VK_NULL_HANDLE;
    s.extent = {0, 0};
}

// ---- frames in flight -------------------------------------------------------------------

bool rv_create_frames(Renderer* r) {
    for (u32 i = 0; i < kFramesInFlight; ++i) {
        FrameInFlight& f = r->frames[i];
        VkCommandPoolCreateInfo pci{};
        pci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = r->queue_family;
        VK_CHECK(vkCreateCommandPool(r->device, &pci, nullptr, &f.pool));

        VkCommandBufferAllocateInfo cai{};
        cai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool        = f.pool;
        cai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(r->device, &cai, &f.cmd));

        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT; // the first wait passes
        VK_CHECK(vkCreateFence(r->device, &fci, nullptr, &f.fence));

        VkSemaphoreCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VK_CHECK(vkCreateSemaphore(r->device, &sci, nullptr, &f.acquire));
    }
    return true;
}

// Device idle only: whatever the frames still hold as garbage can go now.
void rv_destroy_frames(Renderer* r) {
    for (u32 i = 0; i < kFramesInFlight; ++i) {
        FrameInFlight& f = r->frames[i];
        rv_run_garbage(r, f);
        f.garbage.free();
        f.dirty_slots.free();
        if (f.acquire) vkDestroySemaphore(r->device, f.acquire, nullptr);
        if (f.fence)   vkDestroyFence(r->device, f.fence, nullptr);
        if (f.pool)    vkDestroyCommandPool(r->device, f.pool, nullptr); // frees the command buffer
        rv_destroy_buffer(r, f.layers);
        f = FrameInFlight{};
    }
}

} // namespace kv
