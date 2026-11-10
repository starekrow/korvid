// Korvid renderer-vulkan: Vulkan entry points loaded at runtime through the loader that
// vk_platform_load found, so nothing links the loader. VK_NO_PROTOTYPES turns the header into types
// only; the function names below are our own globals with the usual names.
// This header and the sources next to it are the only place a Vulkan header is included.
#pragma once
#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

// Entry points reachable before an instance exists.
#define KV_VK_GLOBAL_FUNCS(X)                      \
    X(vkEnumerateInstanceVersion)                  \
    X(vkEnumerateInstanceLayerProperties)          \
    X(vkEnumerateInstanceExtensionProperties)      \
    X(vkCreateInstance)

// Entry points loaded from the instance.
#define KV_VK_INSTANCE_FUNCS(X)                    \
    X(vkDestroyInstance)                           \
    X(vkEnumeratePhysicalDevices)                  \
    X(vkGetPhysicalDeviceProperties)               \
    X(vkGetPhysicalDeviceProperties2)              \
    X(vkGetPhysicalDeviceFeatures)                 \
    X(vkGetPhysicalDeviceFeatures2)                \
    X(vkGetPhysicalDeviceQueueFamilyProperties)    \
    X(vkGetPhysicalDeviceMemoryProperties)         \
    X(vkEnumerateDeviceExtensionProperties)        \
    X(vkCreateDevice)                              \
    X(vkGetDeviceProcAddr)                         \
    X(vkDestroySurfaceKHR)                         \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)   \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)        \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)   \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)

// Optional instance entry points (VK_EXT_debug_utils); null when the extension is off.
#define KV_VK_INSTANCE_DEBUG_FUNCS(X)              \
    X(vkCreateDebugUtilsMessengerEXT)              \
    X(vkDestroyDebugUtilsMessengerEXT)             \
    X(vkCmdBeginDebugUtilsLabelEXT)                \
    X(vkCmdEndDebugUtilsLabelEXT)                  \
    X(vkSetDebugUtilsObjectNameEXT)

// Entry points loaded from the device.
#define KV_VK_DEVICE_FUNCS(X)                      \
    X(vkDestroyDevice)                             \
    X(vkGetDeviceQueue)                            \
    X(vkDeviceWaitIdle)                            \
    X(vkQueueWaitIdle)                             \
    X(vkCreateSwapchainKHR)                        \
    X(vkDestroySwapchainKHR)                       \
    X(vkGetSwapchainImagesKHR)                     \
    X(vkAcquireNextImageKHR)                       \
    X(vkQueuePresentKHR)                           \
    X(vkCreateImage)                               \
    X(vkDestroyImage)                              \
    X(vkCreateImageView)                           \
    X(vkDestroyImageView)                          \
    X(vkCreateBuffer)                              \
    X(vkDestroyBuffer)                             \
    X(vkGetBufferMemoryRequirements)               \
    X(vkGetImageMemoryRequirements)                \
    X(vkAllocateMemory)                            \
    X(vkFreeMemory)                                \
    X(vkBindBufferMemory)                          \
    X(vkBindImageMemory)                           \
    X(vkMapMemory)                                 \
    X(vkUnmapMemory)                               \
    X(vkCreateSampler)                             \
    X(vkDestroySampler)                            \
    X(vkCreateDescriptorSetLayout)                 \
    X(vkDestroyDescriptorSetLayout)                \
    X(vkCreateDescriptorPool)                      \
    X(vkDestroyDescriptorPool)                     \
    X(vkAllocateDescriptorSets)                    \
    X(vkUpdateDescriptorSets)                      \
    X(vkCreatePipelineLayout)                      \
    X(vkDestroyPipelineLayout)                     \
    X(vkCreateShaderModule)                        \
    X(vkDestroyShaderModule)                       \
    X(vkCreateGraphicsPipelines)                   \
    X(vkCreateComputePipelines)                    \
    X(vkDestroyPipeline)                           \
    X(vkCreateCommandPool)                         \
    X(vkDestroyCommandPool)                        \
    X(vkResetCommandPool)                          \
    X(vkAllocateCommandBuffers)                    \
    X(vkBeginCommandBuffer)                        \
    X(vkEndCommandBuffer)                          \
    X(vkCreateFence)                               \
    X(vkDestroyFence)                              \
    X(vkWaitForFences)                             \
    X(vkResetFences)                               \
    X(vkCreateSemaphore)                           \
    X(vkDestroySemaphore)                          \
    X(vkQueueSubmit2)                              \
    X(vkCmdPipelineBarrier2)                       \
    X(vkCmdBeginRendering)                         \
    X(vkCmdEndRendering)                           \
    X(vkCmdBindPipeline)                           \
    X(vkCmdBindDescriptorSets)                     \
    X(vkCmdPushConstants)                          \
    X(vkCmdSetViewport)                            \
    X(vkCmdSetScissor)                             \
    X(vkCmdDraw)                                   \
    X(vkCmdDrawIndirect)                           \
    X(vkCmdDispatch)                               \
    X(vkCmdCopyBuffer)                             \
    X(vkCmdCopyBufferToImage)                      \
    X(vkCmdCopyImageToBuffer)                      \
    X(vkCreateQueryPool)                           \
    X(vkDestroyQueryPool)                          \
    X(vkCmdResetQueryPool)                         \
    X(vkCmdWriteTimestamp2)                        \
    X(vkCmdBeginQuery)                             \
    X(vkCmdEndQuery)                               \
    X(vkGetQueryPoolResults)

#define KV_VK_DECLARE(name) extern PFN_##name name;
KV_VK_GLOBAL_FUNCS(KV_VK_DECLARE)
KV_VK_INSTANCE_FUNCS(KV_VK_DECLARE)
KV_VK_INSTANCE_DEBUG_FUNCS(KV_VK_DECLARE)
KV_VK_DEVICE_FUNCS(KV_VK_DECLARE)
#undef KV_VK_DECLARE

extern PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;

namespace kv {

// Each returns false if a required entry point is missing.
bool vk_load_global(PFN_vkGetInstanceProcAddr get_instance_proc_addr);
bool vk_load_instance(VkInstance instance, bool debug_utils);
bool vk_load_device(VkDevice device);
void vk_unload();

const char* vk_result_name(VkResult r);

} // namespace kv
