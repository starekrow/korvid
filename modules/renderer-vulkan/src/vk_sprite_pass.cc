// Korvid renderer-vulkan: the sprite pass. Shaders and pipelines, and the frame's
// commands: texture uploads and table copies out of the frame ring, the compute sort
// (count, scan, bucket scan, scatter), and one indirect draw per run of buckets.
//
// A frame draws what the tables say. The CPU's part is the dirty rows it copies in and
// the list of non-empty buckets it keeps; it never visits a sprite to draw.
#include "renderer_internal.h"
#include "korvid/core/arena.h"
#include "korvid/platform/io.h"
#include "korvid/core/format.h"
#include "korvid/core/profile.h"
#include "korvid/core/time.h"
#include "korvid/math/scalar.h"
#include <string.h>

namespace kv {

// Reads shaders/<name> from the program's own files: beside the executable, or inside an
// Android package. SPIR-V is precompiled by the build; nothing compiles here.
bool rb_load_shader(Renderer* r, const char* name, VkShaderModule* out) {
    char path[1024];
    format(path, sizeof path, "shaders/%s", name);
    IoBytes file; // needed only until the module is created
    if (!io_read_bundled(path, &file)) { KV_ERROR("vulkan", "cannot read shader '%s'", path); return false; }
    if (file.size == 0 || (file.size % 4) != 0) { file.free(); KV_ERROR("vulkan", "bad SPIR-V size in '%s'", path); return false; }
    const u32* words = (const u32*)file.data;
    usize len = file.size;

    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = (usize)len;
    ci.pCode    = words;
    VkResult res = vkCreateShaderModule(r->device, &ci, nullptr, out);
    file.free();
    if (res != VK_SUCCESS) { KV_ERROR("vulkan", "vkCreateShaderModule: %s", vk_result_name(res)); return false; }
    KV_INFO("vulkan", "shader '%s' loaded (%llu bytes)", name, (unsigned long long)len);
    return true;
}

namespace {

VkPipelineColorBlendAttachmentState blend_state(BlendMode mode) {
    VkPipelineColorBlendAttachmentState s{};
    s.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                       VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    switch (mode) {
    case BlendMode::Opaque:
        s.blendEnable = VK_FALSE;
        break;
    case BlendMode::Alpha:
        s.blendEnable         = VK_TRUE;
        s.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        s.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        s.colorBlendOp        = VK_BLEND_OP_ADD;
        s.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        s.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        s.alphaBlendOp        = VK_BLEND_OP_ADD;
        break;
    case BlendMode::Additive:
        s.blendEnable         = VK_TRUE;
        s.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        s.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        s.colorBlendOp        = VK_BLEND_OP_ADD;
        s.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        s.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        s.alphaBlendOp        = VK_BLEND_OP_ADD;
        break;
    default:
        break;
    }
    return s;
}

bool create_compute(Renderer* r, const char* entry, VkPipeline* out) {
    VkComputePipelineCreateInfo ci{};
    ci.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = r->sort_shader;
    ci.stage.pName  = entry;
    ci.layout       = r->pipe_layout;
    VK_CHECK(vkCreateComputePipelines(r->device, VK_NULL_HANDLE, 1, &ci, nullptr, out));
    rb_name(r, VK_OBJECT_TYPE_PIPELINE, (u64)*out, entry);
    return true;
}

// A barrier between buffer-writing stages: everything written by `src` is visible to `dst`.
void buffer_barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                    VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkMemoryBarrier2 mb{};
    mb.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    mb.srcStageMask  = src_stage;
    mb.srcAccessMask = src_access;
    mb.dstStageMask  = dst_stage;
    mb.dstAccessMask = dst_access;
    VkDependencyInfo dep{};
    dep.sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers    = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);
}

// ---- uploads --------------------------------------------------------------------------------

void record_texture_uploads(Renderer* r, VkCommandBuffer cmd) {
    for (const PendingUpload& up : r->uploads) {
        // The slot may have been destroyed or re-uploaded since: then this copy has no target.
        if (!r->textures.is_alive(up.slot) || r->textures.items[up.slot].image != up.image) continue;

        VkImageMemoryBarrier2 to_dst{};
        to_dst.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        to_dst.srcStageMask        = VK_PIPELINE_STAGE_2_NONE;
        to_dst.srcAccessMask       = 0;
        to_dst.dstStageMask        = VK_PIPELINE_STAGE_2_COPY_BIT;
        to_dst.dstAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        to_dst.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        to_dst.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_dst.image               = up.image;
        to_dst.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{};
        dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers    = &to_dst;
        vkCmdPipelineBarrier2(cmd, &dep);

        VkBufferImageCopy region{};
        region.bufferOffset     = up.offset;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent      = {up.width, up.height, 1};
        vkCmdCopyBufferToImage(cmd, up.src, up.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        VkImageMemoryBarrier2 to_read = to_dst;
        to_read.srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
        to_read.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        to_read.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        to_read.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        to_read.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_read.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        dep.pImageMemoryBarriers = &to_read;
        vkCmdPipelineBarrier2(cmd, &dep);
    }
    r->uploads.clear();
}

// ---- the sort -------------------------------------------------------------------------------

// The four kernels in rc_sort_steps' order, each one's writes seen by the next, then the
// key sort for the buckets that hold keyed rows; the outputs readied for the draws.
void record_sort(Renderer* r, FrameInFlight& f, VkCommandBuffer cmd) {
    const VkPipelineStageFlags2 cs = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    const VkAccessFlags2 rw = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    SortStep steps[kSortSteps];
    rc_sort_steps(r, steps);
    const VkPipeline pipes[kSortSteps] = {r->cs_count, r->cs_scan, r->cs_bucket_scan, r->cs_scatter};

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->pipe_layout, 0, 1, &f.tables, 0, nullptr);
    for (u32 i = 0; i < kSortSteps; ++i) {
        if (i) buffer_barrier(cmd, cs, rw, cs, rw);
        rb_label_begin(r, cmd, steps[i].label);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipes[i]);
        vkCmdDispatch(cmd, steps[i].groups, 1, 1);
        rb_label_end(r, cmd);
    }
    // Buckets with keyed rows: one group each sorts its range by key. The rest stay in row
    // order and cost nothing here.
    bool keyed = false;
    for (u32 b = 0; rw_next_keyed(r->world, &b); ++b) {
        if (!keyed) {
            buffer_barrier(cmd, cs, rw, cs, rw);
            rb_label_begin(r, cmd, "key sort");
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->cs_key_sort);
            keyed = true;
        }
        vkCmdPushConstants(cmd, r->pipe_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
                           offsetof(PushData, key_bucket), sizeof(u32), &b);
        vkCmdDispatch(cmd, 1, 1, 1);
    }
    if (keyed) rb_label_end(r, cmd);
    buffer_barrier(cmd, cs, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                   VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}

// ---- the sort check -------------------------------------------------------------------------------

void record_sort_check(Renderer* r, VkCommandBuffer cmd) {
    u32 rows = r->world.sprites.len;
    VkDeviceSize meta_bytes = sizeof(u32) * (2 * kBucketCount + 4);
    VkDeviceSize bytes = sizeof(u32) * (VkDeviceSize)rows * 2 + meta_bytes;
    if (r->check_buffer.buffer) rb_defer_destroy_buffer(r, r->check_buffer);
    if (!rb_create_host_buffer(r, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, r->check_buffer)) { r->check_pending = false; return; }
    buffer_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferCopy c0{0, 0, sizeof(u32) * rows};
    VkBufferCopy c1{0, sizeof(u32) * rows, sizeof(u32) * rows};
    VkBufferCopy c2{0, sizeof(u32) * (VkDeviceSize)rows * 2, meta_bytes};
    vkCmdCopyBuffer(cmd, r->tables.sorted.buffer, r->check_buffer.buffer, 1, &c0);
    vkCmdCopyBuffer(cmd, r->tables.bucket_of.buffer, r->check_buffer.buffer, 1, &c1);
    vkCmdCopyBuffer(cmd, r->sort_meta.buffer, r->check_buffer.buffer, 1, &c2);
    r->check_rows     = rows;
    r->check_pending  = false;
    r->check_recorded = true;
}

// ---- screenshots ---------------------------------------------------------------------------------

// Copies the finished color attachment into a host buffer, leaving the image in the layout
// the present barrier after it expects.
void record_screenshot(Renderer* r, VkCommandBuffer cmd, VkImage image, VkExtent2D extent) {
    VkDeviceSize bytes = (VkDeviceSize)extent.width * extent.height * 4;
    if (r->shot_buffer.buffer) rb_defer_destroy_buffer(r, r->shot_buffer);
    if (!rb_create_host_buffer(r, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, r->shot_buffer)) { r->shot_pending = false; return; }
    r->shot_width  = extent.width;
    r->shot_height = extent.height;
    r->shot_rgba   = !(r->swapchain.format == VK_FORMAT_B8G8R8A8_SRGB || r->swapchain.format == VK_FORMAT_B8G8R8A8_UNORM);

    VkImageMemoryBarrier2 to_src{};
    to_src.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    to_src.srcStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    to_src.srcAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    to_src.dstStageMask        = VK_PIPELINE_STAGE_2_COPY_BIT;
    to_src.dstAccessMask       = VK_ACCESS_2_TRANSFER_READ_BIT;
    to_src.oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_src.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.image               = image;
    to_src.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{};
    dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers    = &to_src;
    vkCmdPipelineBarrier2(cmd, &dep);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent      = {extent.width, extent.height, 1};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r->shot_buffer.buffer, 1, &region);

    VkImageMemoryBarrier2 back = to_src;
    back.srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
    back.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    back.dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    back.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    back.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    dep.pImageMemoryBarriers = &back;
    vkCmdPipelineBarrier2(cmd, &dep);
    r->shot_pending  = false;
    r->shot_recorded = true;
}

// ---- the draws ---------------------------------------------------------------------------------

} // namespace

// One indirect draw for a run of buckets (rw_next_run): buckets in it with nothing in them
// draw zero instances. Without multi-draw, one call per non-empty bucket. A game slot whose
// pipeline failed draws with its blend's engine pipeline.
u32 rb_draw_run(Renderer* r, FrameInFlight& f, const DrawRun& run) {
    VkCommandBuffer cmd = f.cmd;
    u32 draws = 0;
    u32 slot = run.slot, first = run.first, last = run.last;
    const GamePipe* gp = slot >= kBlendCount ? &r->game_pipes[slot - kBlendCount] : nullptr;
    VkPipeline pipe = gp ? (gp->pipeline ? gp->pipeline : r->pipelines[(u32)gp->blend]) : r->pipelines[slot];
    // Its label: the layers it covers and its blend, for a capture tool.
    char label[64];
    if (r->debug_utils) {
        rw_run_label(label, sizeof label, run, gp ? name_str(gp->shader) : blend_mode_name((BlendMode)slot));
        rb_label_begin(r, cmd, label);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    if (r->multi_draw_indirect) {
        vkCmdDrawIndirect(cmd, r->indirect.buffer, (VkDeviceSize)first * sizeof(VkDrawIndirectCommand),
                          last - first + 1, sizeof(VkDrawIndirectCommand));
        ++draws;
    } else {
        for (u32 n = first; n <= last; ++n) {
            if (!r->world.bucket_counts[n]) continue;
            vkCmdDrawIndirect(cmd, r->indirect.buffer, (VkDeviceSize)n * sizeof(VkDrawIndirectCommand), 1, sizeof(VkDrawIndirectCommand));
            ++draws;
        }
    }
    if (r->debug_utils) rb_label_end(r, cmd);
    return draws;
}

// ---- shaders and pipelines --------------------------------------------------------------------

bool rb_load_shaders(Renderer* r) {
    return rb_load_shader(r, "sprite.spv", &r->sprite_shader) && rb_load_shader(r, "sprite_sort.spv", &r->sort_shader);
}

bool rb_create_compute_pipelines(Renderer* r) {
    return create_compute(r, "cs_count", &r->cs_count) && create_compute(r, "cs_scan", &r->cs_scan) &&
           create_compute(r, "cs_bucket_scan", &r->cs_bucket_scan) && create_compute(r, "cs_scatter", &r->cs_scatter) &&
           create_compute(r, "cs_key_sort", &r->cs_key_sort);
}

// A sprite pipeline: the engine's vertex shader, a fragment shader (the engine's or a
// game's, entry point fs_main), a blend. Needs the swapchain format. Null on failure.
VkPipeline make_sprite_pipeline(Renderer* r, VkShaderModule fragment, BlendMode mode, const char* label) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = r->sprite_shader;
    stages[0].pName  = "vs_main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    stages[1].pName  = "fs_main";
    VkPipelineVertexInputStateCreateInfo vertex_input{}; // no vertex buffers at all
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

    VkPipelineColorBlendAttachmentState attachment = blend_state(mode);
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
    ci.layout              = r->pipe_layout;
    VkPipeline out = VK_NULL_HANDLE;
    VkResult res = vkCreateGraphicsPipelines(r->device, VK_NULL_HANDLE, 1, &ci, nullptr, &out);
    if (res != VK_SUCCESS) { KV_ERROR("vulkan", "pipeline %s: %s", label, vk_result_name(res)); return VK_NULL_HANDLE; }
    rb_name(r, VK_OBJECT_TYPE_PIPELINE, (u64)out, label);
    return out;
}

// One graphics pipeline per blend mode with the engine's fragment shader, and one per game
// pipe in use; everything else is shared. Needs the swapchain format.
bool rb_create_pipelines(Renderer* r) {
    for (u32 i = 0; i < kBlendCount; ++i) {
        r->pipelines[i] = make_sprite_pipeline(r, r->sprite_shader, (BlendMode)i, blend_mode_name((BlendMode)i));
        if (!r->pipelines[i]) return false;
    }
    for (GamePipe& gp : r->game_pipes)
        if (gp.used && !gp.failed) rb_create_game_pipeline(r, gp);
    rb_post_create_pipeline(r);
    return true;
}

bool rb_create_game_pipeline(Renderer* r, GamePipe& gp) {
    if (gp.failed || !gp.module) return false;
    if (gp.pipeline || !r->swapchain.handle) return true; // made, or to be made with the format
    char label[96];
    format(label, sizeof label, "%s %s", name_str(gp.shader), blend_mode_name(gp.blend));
    gp.pipeline = make_sprite_pipeline(r, gp.module, gp.blend, label);
    if (!gp.pipeline) {
        gp.failed = true;
        KV_ERROR("vulkan", "game shader '%s' does not link with the sprite shader: its sprites draw with the engine's", name_str(gp.shader));
    }
    return gp.pipeline != VK_NULL_HANDLE;
}

bool rb_game_pipe_load(Renderer* r, GamePipe& gp) {
    char file[96];
    format(file, sizeof file, "%s" KV_RB_SHADER_EXT, name_str(gp.shader));
    if (!rb_load_shader(r, file, &gp.module)) return false;
    rb_create_game_pipeline(r, gp);
    return true;
}

// A format change: the pipelines in flight retire with the frame.
void rb_defer_destroy_pipelines(Renderer* r) {
    for (u32 i = 0; i < kBlendCount; ++i) {
        rb_defer_destroy(r, Garbage::PipelineKind, (u64)r->pipelines[i]);
        r->pipelines[i] = VK_NULL_HANDLE;
    }
    for (GamePipe& gp : r->game_pipes) {
        if (gp.pipeline) rb_defer_destroy(r, Garbage::PipelineKind, (u64)gp.pipeline);
        gp.pipeline = VK_NULL_HANDLE;
    }
    rb_post_defer_destroy_pipeline(r);
}

void rb_destroy_pipelines_now(Renderer* r) {
    for (u32 i = 0; i < kBlendCount; ++i) {
        if (r->pipelines[i]) vkDestroyPipeline(r->device, r->pipelines[i], nullptr);
        r->pipelines[i] = VK_NULL_HANDLE;
    }
    VkPipeline* cs[] = {&r->cs_count, &r->cs_scan, &r->cs_bucket_scan, &r->cs_scatter, &r->cs_key_sort};
    for (VkPipeline* p : cs) { if (*p) vkDestroyPipeline(r->device, *p, nullptr); *p = VK_NULL_HANDLE; }
    for (GamePipe& gp : r->game_pipes) {
        if (gp.pipeline) vkDestroyPipeline(r->device, gp.pipeline, nullptr);
        if (gp.module) vkDestroyShaderModule(r->device, gp.module, nullptr);
        gp = GamePipe{};
    }
    if (r->sprite_shader) vkDestroyShaderModule(r->device, r->sprite_shader, nullptr);
    if (r->sort_shader)   vkDestroyShaderModule(r->device, r->sort_shader, nullptr);
    r->sprite_shader = r->sort_shader = VK_NULL_HANDLE;
}

// ---- the frame -----------------------------------------------------------------------------------

namespace {

const VkShaderStageFlags k_push_stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

void image_barrier(VkCommandBuffer cmd, VkImage image, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkImageLayout from,
                   VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access, VkImageLayout to) {
    VkImageMemoryBarrier2 b{};
    b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask        = src_stage;
    b.srcAccessMask       = src_access;
    b.dstStageMask        = dst_stage;
    b.dstAccessMask       = dst_access;
    b.oldLayout           = from;
    b.newLayout           = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = image;
    b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{};
    dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers    = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void set_viewport(Renderer* r, VkCommandBuffer cmd) {
    const VkExtent2D e = r->swapchain.extent;
    VkViewport vp{0.0f, 0.0f, (f32)e.width, (f32)e.height, 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, e};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

} // namespace

// The steps of the core's frame (korvid/render/core/frame.inl), in the order it calls them.
// One command buffer holds the frame; f.image is the swapchain image it draws into.

bool rb_record_begin(Renderer* r, FrameInFlight& f) {
    VK_CHECK(vkResetCommandPool(r->device, f.pool, 0));
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(f.cmd, &bi));
    rb_gpu_frame_begin(r, f, f.cmd);
    f.gpu_pixels = f.have_image ? (u64)f.width * f.height : 0;
    f.push_stale = false;
    return true;
}

// Textures, then the tables opened for the rows the ticks changed. The previous frame read
// the tables in its compute and vertex stages and wrote them in its copies; both must be
// done before this frame's copies land.
void rb_upload_begin(Renderer* r, FrameInFlight& f) {
    rb_label_begin(r, f.cmd, "upload");
    record_texture_uploads(r, f.cmd);
    buffer_barrier(f.cmd, VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                          VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
}

void rb_copy_rows(Renderer*, FrameInFlight& f, RbBuffer& table, const RingSpan& span, u64 offset, u64 bytes) {
    VkBufferCopy region{span.offset, offset, bytes};
    vkCmdCopyBuffer(f.cmd, span.buffer.buffer, table.buffer, 1, &region);
}

void rb_upload_end(Renderer* r, FrameInFlight& f) {
    buffer_barrier(f.cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    rb_label_end(r, f.cmd);
    rb_gpu_mark(r, f, f.cmd, kGpuUploaded);
    rb_gpu_stats_begin(r, f, f.cmd); // the sort's and the drawing's shader invocations
}

// The push block is pushed once here, for the sort and for the draws after it.
void rb_sort(Renderer* r, FrameInFlight& f, const PushData& push) {
    vkCmdPushConstants(f.cmd, r->pipe_layout, k_push_stages, 0, sizeof push, &push);
    rb_label_begin(r, f.cmd, "sort");
    record_sort(r, f, f.cmd);
    rb_label_end(r, f.cmd);
}

void rb_read_sort(Renderer* r, FrameInFlight& f) { record_sort_check(r, f.cmd); }

void rb_sort_end(Renderer* r, FrameInFlight& f, bool) { rb_gpu_mark(r, f, f.cmd, kGpuSorted); }

void rb_pass_begin(Renderer* r, FrameInFlight& f, RcPass pass, const RenderView& view) {
    VkCommandBuffer cmd = f.cmd;
    if (pass == RcPass::World) {
        rb_label_begin(r, cmd, "world");
        rb_post_begin_world(r, cmd, view);
        return;
    }
    const Swapchain& s = r->swapchain;
    // Undefined -> color attachment. Waiting on the acquire semaphore happens at
    // COLOR_ATTACHMENT_OUTPUT, so that is where the barrier starts too.
    image_barrier(cmd, s.images[f.image], VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color{};
    color.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView   = s.views[f.image];
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{view.clear_color.r, view.clear_color.g, view.clear_color.b, view.clear_color.a}};
    VkRenderingInfo ri{};
    ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea           = {{0, 0}, s.extent};
    ri.layerCount           = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments    = &color;
    rb_label_begin(r, cmd, "draw");
    vkCmdBeginRendering(cmd, &ri);
}

// The post pass has its own layout: it disturbs the sprites' sets and push block, which
// rb_bind_sprites then puts back.
void rb_pass_post(Renderer* r, FrameInFlight& f, const RenderView& view) {
    set_viewport(r, f.cmd);
    rb_post_draw(r, f, f.cmd, view);
    f.push_stale = true;
}

void rb_bind_sprites(Renderer* r, FrameInFlight& f, const PushData& push) {
    set_viewport(r, f.cmd);
    VkDescriptorSet sets[2] = {f.tables, f.textures};
    vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipe_layout, 0, 2, sets, 0, nullptr);
    if (f.push_stale) {
        vkCmdPushConstants(f.cmd, r->pipe_layout, k_push_stages, 0, sizeof push, &push);
        f.push_stale = false;
    }
}

void rb_pass_end(Renderer* r, FrameInFlight& f, RcPass pass) {
    if (pass == RcPass::World) rb_post_end_world(r, f.cmd);
    else vkCmdEndRendering(f.cmd);
    rb_label_end(r, f.cmd);
}

void rb_read_window(Renderer* r, FrameInFlight& f) { record_screenshot(r, f.cmd, r->swapchain.images[f.image], r->swapchain.extent); }

bool rb_record_end(Renderer* r, FrameInFlight& f) {
    if (f.have_image)
        image_barrier(f.cmd, r->swapchain.images[f.image], VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    rb_gpu_stats_end(r, f, f.cmd);
    rb_gpu_mark(r, f, f.cmd, kGpuDrawn);
    rb_gpu_mark(r, f, f.cmd, kGpuEnd);
    VK_CHECK(vkEndCommandBuffer(f.cmd));
    return true;
}

} // namespace kv
