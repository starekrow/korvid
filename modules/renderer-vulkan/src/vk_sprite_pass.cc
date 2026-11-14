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

// Dirty sprite rows and material rows, staged into the ring and copied into the tables.
bool record_table_copies(Renderer* r, VkCommandBuffer cmd) {
    bool ok = true;
    u32 rows = rw_dirty_runs(r->world.sprite_dirty, r->world.sprites.len, 32, [&](u32 a, u32 b) {
        VkDeviceSize bytes = (VkDeviceSize)(b - a) * sizeof(GpuSprite);
        RingSpan span = rb_ring_alloc(r, bytes, 16);
        if (!span.ptr) { ok = false; return; }
        GpuSprite* dst = (GpuSprite*)span.ptr;
        rw_stage_sprites(r->world, a, b, dst);
        VkBufferCopy region{span.offset, (VkDeviceSize)a * sizeof(GpuSprite), bytes};
        vkCmdCopyBuffer(cmd, span.buffer, r->tables.sprites.buffer, 1, &region);
    });
    r->rows_uploaded += rows;
    KV_COUNTER("sprite rows uploaded", rows);

    rw_dirty_runs(r->world.material_dirty, kMaxMaterials, 8, [&](u32 a, u32 b) {
        VkDeviceSize bytes = (VkDeviceSize)(b - a) * sizeof(GpuMaterial);
        RingSpan span = rb_ring_alloc(r, bytes, 16);
        if (!span.ptr) { ok = false; return; }
        GpuMaterial* dst = (GpuMaterial*)span.ptr;
        for (u32 i = a; i < b; ++i) rc_material_row(r, i, dst[i - a]);
        VkBufferCopy region{span.offset, (VkDeviceSize)a * sizeof(GpuMaterial), bytes};
        vkCmdCopyBuffer(cmd, span.buffer, r->material_table.buffer, 1, &region);
    });
    return ok;
}

// ---- the sort -------------------------------------------------------------------------------

void record_sort(Renderer* r, FrameInFlight& f, VkCommandBuffer cmd) {
    u32 blocks = (r->world.sprites.len + kBlockRows - 1) / kBlockRows;
    const VkPipelineStageFlags2 cs = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    const VkAccessFlags2 rw = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->pipe_layout, 0, 1, &f.tables, 0, nullptr);
    rb_label_begin(r, cmd, "count");
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->cs_count);
    vkCmdDispatch(cmd, blocks, 1, 1);
    rb_label_end(r, cmd);
    buffer_barrier(cmd, cs, rw, cs, rw);
    rb_label_begin(r, cmd, "scan");
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->cs_scan);
    vkCmdDispatch(cmd, kBucketCount / kComputeGroup, 1, 1);
    rb_label_end(r, cmd);
    buffer_barrier(cmd, cs, rw, cs, rw);
    rb_label_begin(r, cmd, "bucket scan");
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->cs_bucket_scan);
    vkCmdDispatch(cmd, 1, 1, 1);
    rb_label_end(r, cmd);
    buffer_barrier(cmd, cs, rw, cs, rw);
    rb_label_begin(r, cmd, "scatter");
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->cs_scatter);
    vkCmdDispatch(cmd, blocks, 1, 1);
    rb_label_end(r, cmd);
    // Buckets with keyed rows: one group each sorts its range by key. The rest stay in row
    // order and cost nothing here.
    bool keyed = false;
    for (u32 b = 0; b < kBucketCount; ++b) {
        if (!r->world.keyed_counts[b] || !r->world.bucket_counts[b]) continue;
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

// Copies sorted[], bucket_of[] and the sort meta into a host buffer, after the sort.
// Each row's key as this frame computes it (cs_key_sort's formula on the CPU's copy of the
// rows), kept for the check: the rows may change before the frame's fence passes.
void snapshot_keys(Renderer* r, f32 alpha) {
    u32 rows = r->world.sprites.len;
    free_in(mem_general(), r->check_keys, sizeof(f32) * (r->check_rows ? r->check_rows : 1));
    r->check_keys = (f32*)alloc_in(mem_general(), sizeof(f32) * (rows ? rows : 1));
    rw_snapshot_keys(r->world, alpha, r->check_keys);
}

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

// One indirect draw per run of buckets in [first_bucket, end_bucket): from a non-empty
// bucket to the last non-empty bucket of the same pipeline slot before one of another slot
// intervenes. Buckets in between with nothing in them draw zero instances. Without
// multi-draw, one call per non-empty bucket. A game slot whose pipeline failed draws with
// its blend's engine pipeline.
u32 rb_record_draws(Renderer* r, VkCommandBuffer cmd, u32 first_bucket, u32 end_bucket) {
    u32 draws = 0;
    u32 b = first_bucket;
    DrawRun run;
    while (rw_next_run(r->world.bucket_counts, &b, end_bucket, &run)) {
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
    }
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

u32 rb_material_slot(Renderer* r, Name shader, BlendMode blend) {
    u32 engine = (u32)blend < kBlendCount ? (u32)blend : (u32)BlendMode::Alpha;
    if (shader.is_null()) return engine;
    u32 free_slot = kGameSlots;
    for (u32 i = 0; i < kGameSlots; ++i) {
        GamePipe& gp = r->game_pipes[i];
        if (gp.used && gp.shader == shader && gp.blend == blend) return gp.failed ? engine : kBlendCount + i;
        if (!gp.used && free_slot == kGameSlots) free_slot = i;
    }
    if (free_slot == kGameSlots) {
        KV_ERROR("vulkan", "game shader '%s': all %u game pipeline slots are taken; drawn with the engine's", name_str(shader), kGameSlots);
        return engine;
    }
    GamePipe& gp = r->game_pipes[free_slot];
    gp = GamePipe{};
    gp.used   = true;
    gp.shader = shader;
    gp.blend  = blend;
    char file[96];
    format(file, sizeof file, "%s.spv", name_str(shader));
    if (!rb_load_shader(r, file, &gp.module)) {
        gp.failed = true;
        KV_ERROR("vulkan", "game shader '%s' did not load: its sprites draw with the engine's", name_str(shader));
        return engine;
    }
    rb_create_game_pipeline(r, gp);
    return gp.failed ? engine : kBlendCount + free_slot;
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

// Every bucket's run in sorted[] holds exactly the rows whose bucket it is, in ascending
// row order, once each; the counts agree with the CPU's where nothing was culled.
bool rb_verify_sort(Renderer* r) {
    r->check_recorded = false;
    if (!r->check_buffer.mapped) return false;
    bool ok = rw_verify_sort(r->world, (const u32*)r->check_buffer.mapped, r->check_rows, r->check_keys, KV_RB_NAME);
    if (ok) ++r->checks_passed; else ++r->checks_failed;
    rb_defer_destroy_buffer(r, r->check_buffer);
    return ok;
}

// Binary PPM, RGB. The swapchain is BGRA or RGBA; sRGB-encoded bytes either way.
bool rb_write_screenshot(Renderer* r) {
    r->shot_recorded = false;
    if (!r->shot_buffer.mapped) return false;
    IoFileWriter f;
    if (!f.open(r->shot_path)) { KV_ERROR("vulkan", "cannot write screenshot '%s'", r->shot_path); rb_defer_destroy_buffer(r, r->shot_buffer); return false; }
    f.print("P6\n%u %u\n255\n", r->shot_width, r->shot_height);
    bool bgra = r->swapchain.format == VK_FORMAT_B8G8R8A8_SRGB || r->swapchain.format == VK_FORMAT_B8G8R8A8_UNORM;
    const u8* px = (const u8*)r->shot_buffer.mapped;
    MemScope scope;
    u8* row = arena_push_n<u8>(*scope.arena, r->shot_width * 3);
    for (u32 y = 0; y < r->shot_height; ++y) {
        const u8* src = px + (usize)y * r->shot_width * 4;
        for (u32 x = 0; x < r->shot_width; ++x, src += 4) {
            row[x * 3 + 0] = bgra ? src[2] : src[0];
            row[x * 3 + 1] = src[1];
            row[x * 3 + 2] = bgra ? src[0] : src[2];
        }
        f.write(row, r->shot_width * 3);
    }
    if (!f.close()) KV_ERROR("vulkan", "screenshot '%s': write failed (errno %d)", r->shot_path, f.error);
    else KV_INFO("vulkan", "screenshot '%s' %ux%u", r->shot_path, r->shot_width, r->shot_height);
    rb_defer_destroy_buffer(r, r->shot_buffer);
    return true;
}

// ---- the frame -----------------------------------------------------------------------------------

bool rb_record_frame(Renderer* r, FrameInFlight& f, bool have_image, u32 image_index, const RenderView& view) {
    KV_ZONE("record");
    Swapchain& s = r->swapchain;
    VkCommandBuffer cmd = f.cmd;

    VK_CHECK(vkResetCommandPool(r->device, f.pool, 0));
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    rb_gpu_frame_begin(r, f, cmd);
    f.gpu_pixels = have_image ? (u64)s.extent.width * s.extent.height : 0;

    // Uploads first: textures, then the rows the ticks changed. The previous frame read
    // the tables in its compute and vertex stages and wrote them in its copies; both must
    // be done before this frame's copies land.
    rb_label_begin(r, cmd, "upload");
    record_texture_uploads(r, cmd);
    buffer_barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                        VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    if (!record_table_copies(r, cmd)) return false;
    buffer_barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    rb_label_end(r, cmd);
    rb_gpu_mark(r, f, cmd, kGpuUploaded);
    rb_gpu_stats_begin(r, f, cmd); // the sort's and the drawing's shader invocations

    bool draw = have_image && r->world.sprites.len > 0;
    // World: move the view's center to the origin, undo the camera's rotation, then the
    // ortho of a rect that size about the origin. The cull rect is the rotated view's
    // bounding box. Screen: the window in pixels about its center, no camera.
    f32 sw = (f32)s.extent.width, sh = (f32)s.extent.height;
    PushData push = rw_push_block(r->world, view, sw, sh, (f32)(time_now() - r->start_time), KV_RB_CLIP_Y_UP != 0);
    vkCmdPushConstants(cmd, r->pipe_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);

    if (draw) {
        rb_label_begin(r, cmd, "sort");
        record_sort(r, f, cmd);
        rb_label_end(r, cmd);
        if (r->check_pending) { snapshot_keys(r, view.alpha); record_sort_check(r, cmd); }
    }
    rb_gpu_mark(r, f, cmd, kGpuSorted);

    if (have_image) {
        // Undefined -> color attachment. Waiting on the acquire semaphore happens at
        // COLOR_ATTACHMENT_OUTPUT, so that is where the barrier starts too.
        VkImageMemoryBarrier2 to_color{};
        to_color.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        to_color.srcStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        to_color.srcAccessMask       = 0;
        to_color.dstStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        to_color.dstAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        to_color.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        to_color.newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        to_color.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_color.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_color.image               = s.images[image_index];
        to_color.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{};
        dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers    = &to_color;
        vkCmdPipelineBarrier2(cmd, &dep);

        // With a post shader, the world (the layers below the post layer) goes into the
        // target first, and the window's rendering begins with the post pass.
        bool post = rb_post_prepare(r);
        u32 split = post ? rw_bucket_of(r->post.layer, 0) : 0;
        VkViewport vp{0.0f, 0.0f, (f32)s.extent.width, (f32)s.extent.height, 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, s.extent};
        VkDescriptorSet sets[2] = {f.tables, f.textures};
        auto bind_sprites = [&]() {
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipe_layout, 0, 2, sets, 0, nullptr);
        };
        u32 draws = 0;
        if (post) {
            rb_label_begin(r, cmd, "world");
            rb_post_begin_world(r, cmd, view);
            if (draw) { bind_sprites(); draws += rb_record_draws(r, cmd, 0, split); }
            rb_post_end_world(r, cmd);
            rb_label_end(r, cmd);
        }

        VkRenderingAttachmentInfo color{};
        color.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color.imageView   = s.views[image_index];
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

        if (post) {
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            rb_post_draw(r, f, cmd, view);
        }
        if (draw) {
            // The post pass's layout disturbed the sprites' sets and push block.
            bind_sprites();
            if (post) vkCmdPushConstants(cmd, r->pipe_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
            draws += rb_record_draws(r, cmd, split, kBucketCount);
        }
        KV_COUNTER("indirect draws", draws);

        vkCmdEndRendering(cmd);
        rb_label_end(r, cmd);

        if (r->shot_pending) record_screenshot(r, cmd, s.images[image_index], s.extent);

        VkImageMemoryBarrier2 to_present = to_color;
        to_present.srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        to_present.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        to_present.dstStageMask  = VK_PIPELINE_STAGE_2_NONE;
        to_present.dstAccessMask = 0;
        to_present.oldLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        to_present.newLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        dep.pImageMemoryBarriers = &to_present;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    rb_gpu_stats_end(r, f, cmd);
    rb_gpu_mark(r, f, cmd, kGpuDrawn);
    rb_gpu_mark(r, f, cmd, kGpuEnd);
    VK_CHECK(vkEndCommandBuffer(cmd));
    return true;
}

} // namespace kv
