// Korvid renderer-vulkan: the full-frame pass. With a post shader set, the layers below the
// post layer draw into the world's target (the swapchain's size and format, so the sprite
// pipelines draw into it unchanged); the target turns readable; the window's rendering
// begins with one triangle through the game's post shader, and the layers above draw over
// it. One target serves both frames in flight: commands on one queue run in submission
// order, and the barrier that opens a frame's world waits for the last frame's reads.
#include "renderer_internal.h"
#include "korvid/core/time.h"
#include "korvid/core/format.h"

#include <string.h>

namespace kv {

namespace {

// What vs_post and the game's fragment shader share as a push block (post_fragment.slangh).
struct PostPushData {
    f32 size[2];
    f32 time;
    u32 tick;
    u32 count;
    f32 alpha;
    u32 pad0, pad1;
};

// The layouts, the sampler, the sets and the parameter buffers, on first use.
bool post_layout(Renderer* r) {
    PostPass& p = r->post;
    if (p.layout) return true;
    VkDescriptorSetLayoutBinding b[3]{};
    b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;  b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;        b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b[2].binding = 2; b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b[2].descriptorCount = 1; b[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo lci{};
    lci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lci.bindingCount = 3;
    lci.pBindings    = b;
    VK_CHECK(vkCreateDescriptorSetLayout(r->device, &lci, nullptr, &p.set_layout));

    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PostPushData)};
    VkPipelineLayoutCreateInfo plci{};
    plci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount         = 1;
    plci.pSetLayouts            = &p.set_layout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges    = &range;
    VK_CHECK(vkCreatePipelineLayout(r->device, &plci, nullptr, &p.layout));

    VkDescriptorPoolSize sizes[3] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kFramesInFlight},
        {VK_DESCRIPTOR_TYPE_SAMPLER, kFramesInFlight},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kFramesInFlight},
    };
    VkDescriptorPoolCreateInfo pci{};
    pci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.maxSets       = kFramesInFlight;
    pci.poolSizeCount = 3;
    pci.pPoolSizes    = sizes;
    VK_CHECK(vkCreateDescriptorPool(r->device, &pci, nullptr, &p.pool));
    VkDescriptorSetLayout layouts[kFramesInFlight];
    for (u32 i = 0; i < kFramesInFlight; ++i) layouts[i] = p.set_layout;
    VkDescriptorSetAllocateInfo ai{};
    ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool     = p.pool;
    ai.descriptorSetCount = kFramesInFlight;
    ai.pSetLayouts        = layouts;
    VK_CHECK(vkAllocateDescriptorSets(r->device, &ai, p.sets));

    VkSamplerCreateInfo sci{};
    sci.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter    = VK_FILTER_LINEAR;
    sci.minFilter    = VK_FILTER_LINEAR;
    sci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; // a bent edge repeats its own pixels
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod       = 1.0f;
    VK_CHECK(vkCreateSampler(r->device, &sci, nullptr, &p.sampler));

    for (u32 i = 0; i < kFramesInFlight; ++i) {
        if (!rv_create_host_buffer(r, sizeof p.params, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, p.param_buffers[i])) return false;
        rv_name(r, VK_OBJECT_TYPE_BUFFER, (u64)p.param_buffers[i].buffer, "post params");
    }
    return true;
}

void post_destroy_target_later(Renderer* r) {
    PostPass& p = r->post;
    if (p.view)   rv_defer_destroy(r, Garbage::ImageViewKind, (u64)p.view);
    if (p.image)  rv_defer_destroy(r, Garbage::ImageKind, (u64)p.image);
    if (p.memory) rv_defer_destroy(r, Garbage::MemoryKind, (u64)p.memory);
    p.view = VK_NULL_HANDLE; p.image = VK_NULL_HANDLE; p.memory = VK_NULL_HANDLE;
    p.extent = {0, 0};
    p.format = VK_FORMAT_UNDEFINED;
}

void post_fail(Renderer* r, const char* why) {
    PostPass& p = r->post;
    p.failed = true;
    KV_ERROR("vulkan", "post shader '%s' %s: the frame is drawn without it", name_str(p.shader), why);
}

} // namespace

bool rv_post_set(Renderer* r, i32 layer, Name shader) {
    PostPass& p = r->post;
    p.layer = layer;
    if (shader == p.shader) return !p.failed;
    // A different shader: the old one's pipeline retires with the frame, its module now
    // (a module is not referenced once its pipelines exist).
    rv_post_defer_destroy_pipeline(r);
    if (p.module) vkDestroyShaderModule(r->device, p.module, nullptr);
    p.module = VK_NULL_HANDLE;
    p.shader = shader;
    p.failed = false;
    if (shader.is_null()) { post_destroy_target_later(r); return true; }

    if (!post_layout(r)) { post_fail(r, "has no layout"); return false; }
    if (!p.vertex && !rv_load_shader(r, "post.spv", &p.vertex)) { post_fail(r, "has no vertex shader (post.spv)"); return false; }
    char file[96];
    format(file, sizeof file, "%s.spv", name_str(shader));
    if (!rv_load_shader(r, file, &p.module)) { post_fail(r, "did not load"); return false; }
    return rv_post_create_pipeline(r);
}

bool rv_post_active(const Renderer* r) {
    const PostPass& p = r->post;
    return !p.shader.is_null() && !p.failed && p.pipeline;
}

// The pipeline: vs_post, the game's fs_main, no blending, the swapchain's format.
bool rv_post_create_pipeline(Renderer* r) {
    PostPass& p = r->post;
    if (p.shader.is_null() || p.failed || !p.module) return false;
    if (p.pipeline || !r->swapchain.handle) return true; // made, or to be made with the format
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = p.vertex;
    stages[0].pName  = "vs_post";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = p.module;
    stages[1].pName  = "fs_main";
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount  = 1;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode    = VK_CULL_MODE_NONE;
    raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth   = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = KV_COUNTOF(dynamic);
    dynamic_state.pDynamicStates    = dynamic;
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount    = 1;
    rendering.pColorAttachmentFormats = &r->swapchain.format;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments    = &attachment;
    VkGraphicsPipelineCreateInfo ci{};
    ci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.pNext               = &rendering;
    ci.stageCount          = 2;
    ci.pStages             = stages;
    ci.pVertexInputState   = &vertex_input;
    ci.pInputAssemblyState = &assembly;
    ci.pViewportState      = &viewport;
    ci.pRasterizationState = &raster;
    ci.pMultisampleState   = &multisample;
    ci.pColorBlendState    = &blend;
    ci.pDynamicState       = &dynamic_state;
    ci.layout              = p.layout;
    VkResult res = vkCreateGraphicsPipelines(r->device, VK_NULL_HANDLE, 1, &ci, nullptr, &p.pipeline);
    if (res != VK_SUCCESS) {
        p.pipeline = VK_NULL_HANDLE;
        KV_ERROR("vulkan", "post pipeline: %s", vk_result_name(res));
        post_fail(r, "does not link with vs_post");
        return false;
    }
    char label[96];
    format(label, sizeof label, "post %s", name_str(p.shader));
    rv_name(r, VK_OBJECT_TYPE_PIPELINE, (u64)p.pipeline, label);
    return true;
}

void rv_post_defer_destroy_pipeline(Renderer* r) {
    if (r->post.pipeline) rv_defer_destroy(r, Garbage::PipelineKind, (u64)r->post.pipeline);
    r->post.pipeline = VK_NULL_HANDLE;
}

// The target for the swapchain as it is now: made again when its size or format moved.
// A format the device cannot both draw into and sample turns the pass off.
bool rv_post_prepare(Renderer* r) {
    PostPass& p = r->post;
    if (!rv_post_active(r)) return false;
    const Swapchain& s = r->swapchain;
    if (p.image && p.extent.width == s.extent.width && p.extent.height == s.extent.height && p.format == s.format) return true;
    post_destroy_target_later(r);

    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(r->gpu, s.format, &fp);
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
                                VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((fp.optimalTilingFeatures & need) != need) { post_fail(r, "cannot sample the swapchain's format"); return false; }

    VkImageCreateInfo ici{};
    ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = s.format;
    ici.extent        = {s.extent.width, s.extent.height, 1};
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(r->device, &ici, nullptr, &p.image));
    rv_name(r, VK_OBJECT_TYPE_IMAGE, (u64)p.image, "world target");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(r->device, p.image, &req);
    VkMemoryAllocateInfo mai{};
    mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize  = req.size;
    mai.memoryTypeIndex = rv_find_memory_type(r, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX) { KV_ERROR("vulkan", "no device-local memory type"); return false; }
    VK_CHECK(vkAllocateMemory(r->device, &mai, nullptr, &p.memory));
    VK_CHECK(vkBindImageMemory(r->device, p.image, p.memory, 0));
    VkImageViewCreateInfo vci{};
    vci.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image            = p.image;
    vci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    vci.format           = s.format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(r->device, &vci, nullptr, &p.view));
    p.extent = s.extent;
    p.format = s.format;
    KV_INFO("vulkan", "world target %ux%u for post shader '%s'", p.extent.width, p.extent.height, name_str(p.shader));
    return true;
}

void rv_post_begin_world(Renderer* r, VkCommandBuffer cmd, const RenderView& view) {
    PostPass& p = r->post;
    // The last frame's post pass read the target; what it held is not wanted.
    VkImageMemoryBarrier2 b{};
    b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask        = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.srcAccessMask       = 0;
    b.dstStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    b.dstAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = p.image;
    b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{};
    dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers    = &b;
    vkCmdPipelineBarrier2(cmd, &dep);

    VkRenderingAttachmentInfo color{};
    color.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView   = p.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{view.clear_color.r, view.clear_color.g, view.clear_color.b, view.clear_color.a}};
    VkRenderingInfo ri{};
    ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea           = {{0, 0}, p.extent};
    ri.layerCount           = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments    = &color;
    vkCmdBeginRendering(cmd, &ri);
}

void rv_post_end_world(Renderer* r, VkCommandBuffer cmd) {
    vkCmdEndRendering(cmd);
    VkImageMemoryBarrier2 b{};
    b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    b.srcAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstStageMask        = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask       = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = r->post.image;
    b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{};
    dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers    = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

// The frame's set (its fence has passed, so nothing reads it): the target, the sampler,
// this frame's parameters. Then one triangle.
void rv_post_draw(Renderer* r, FrameInFlight& f, VkCommandBuffer cmd, const RenderView& view) {
    PostPass& p = r->post;
    u32 fi = (u32)(&f - r->frames);
    Buffer& pb = p.param_buffers[fi];
    memcpy(pb.mapped, p.params, sizeof p.params);

    VkDescriptorImageInfo image{};
    image.imageView   = p.view;
    image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo sampler{};
    sampler.sampler = p.sampler;
    VkDescriptorBufferInfo params{pb.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[3]{};
    for (u32 i = 0; i < 3; ++i) {
        w[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet          = p.sets[fi];
        w[i].dstBinding      = i;
        w[i].descriptorCount = 1;
    }
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;  w[0].pImageInfo  = &image;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;        w[1].pImageInfo  = &sampler;
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[2].pBufferInfo = &params;
    vkUpdateDescriptorSets(r->device, 3, w, 0, nullptr);

    PostPushData push{};
    push.size[0] = (f32)p.extent.width;
    push.size[1] = (f32)p.extent.height;
    push.time    = (f32)(time_now() - r->start_time);
    push.tick    = r->tick;
    push.count   = p.param_count;
    push.alpha   = view.alpha;
    rv_label_begin(r, cmd, "post");
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layout, 0, 1, &p.sets[fi], 0, nullptr);
    vkCmdPushConstants(cmd, p.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    rv_label_end(r, cmd);
}

// Device idle.
void rv_post_destroy_now(Renderer* r) {
    PostPass& p = r->post;
    if (p.pipeline)   vkDestroyPipeline(r->device, p.pipeline, nullptr);
    if (p.module)     vkDestroyShaderModule(r->device, p.module, nullptr);
    if (p.vertex)     vkDestroyShaderModule(r->device, p.vertex, nullptr);
    if (p.view)       vkDestroyImageView(r->device, p.view, nullptr);
    if (p.image)      vkDestroyImage(r->device, p.image, nullptr);
    if (p.memory)     vkFreeMemory(r->device, p.memory, nullptr);
    if (p.sampler)    vkDestroySampler(r->device, p.sampler, nullptr);
    if (p.pool)       vkDestroyDescriptorPool(r->device, p.pool, nullptr);
    if (p.layout)     vkDestroyPipelineLayout(r->device, p.layout, nullptr);
    if (p.set_layout) vkDestroyDescriptorSetLayout(r->device, p.set_layout, nullptr);
    for (Buffer& b : p.param_buffers) rv_destroy_buffer(r, b);
    p = PostPass{};
}

} // namespace kv
