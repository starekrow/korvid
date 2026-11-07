// Korvid renderer-vulkan: the sprite pass. Shaders and pipelines, and the frame's
// commands: texture uploads and table copies out of the frame ring, the compute sort
// (count, scan, bucket scan, scatter), and one indirect draw per run of buckets.
//
// A frame draws what the tables say. The CPU's part is the dirty rows it copies in and
// the list of non-empty buckets it keeps; it never visits a sprite to draw.
#include "renderer_internal.h"
#include "korvid/core/arena.h"
#include "korvid/core/profile.h"
#include <stdio.h>
#include <string.h>

namespace kv {

namespace {

// Reads <exe dir>/shaders/<name>. SPIR-V is precompiled by the build; nothing compiles here.
bool load_shader(Renderer* r, const char* name, VkShaderModule* out) {
    const char* base = SDL_GetBasePath();
    char path[1024];
    snprintf(path, sizeof path, "%sshaders/%s", base ? base : "", name);
    FILE* f = fopen(path, "rb");
    if (!f) { KV_ERROR("vulkan", "cannot open shader '%s'", path); return false; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || (len % 4) != 0) { fclose(f); KV_ERROR("vulkan", "bad SPIR-V size in '%s'", path); return false; }
    MemScope scope; // the words are needed only until the module is created
    u32* words = arena_push_n<u32>(*scope.arena, (u32)(len / 4));
    bool ok = words && fread(words, 1, (usize)len, f) == (usize)len;
    fclose(f);
    if (!ok) { KV_ERROR("vulkan", "short read on '%s'", path); return false; }

    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = (usize)len;
    ci.pCode    = words;
    VkResult res = vkCreateShaderModule(r->device, &ci, nullptr, out);
    if (res != VK_SUCCESS) { KV_ERROR("vulkan", "vkCreateShaderModule: %s", vk_result_name(res)); return false; }
    KV_INFO("vulkan", "shader '%s' loaded (%ld bytes)", name, len);
    return true;
}

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

// Walks a dirty bitmap over [0, rows) and calls copy_run(a, b) for every run of set bits,
// joining runs separated by fewer than `join` clean rows (uploading a clean row again is
// cheaper than another copy region). Clears the bits.
template <class F>
u32 for_dirty_runs(Array<u64>& bits, u32 rows, u32 join, F copy_run) {
    u32 words = (rows + 63) / 64;
    if (words > bits.len) words = bits.len;
    u32 run_a = 0, run_b = 0; // current run [run_a, run_b)
    bool open = false;
    u32 copied = 0;
    for (u32 w = 0; w < words; ++w) {
        u64 word = bits[w];
        if (!word) continue;
        bits[w] = 0;
        while (word) {
            u32 bit = (u32)__builtin_ctzll(word);
            word &= word - 1;
            u32 i = w * 64 + bit;
            if (i >= rows) break;
            if (open && i <= run_b + join) { run_b = i + 1; continue; }
            if (open) { copy_run(run_a, run_b); copied += run_b - run_a; }
            run_a = i; run_b = i + 1; open = true;
        }
    }
    if (open) { copy_run(run_a, run_b); copied += run_b - run_a; }
    return copied;
}

// Dirty sprite rows and material rows, staged into the ring and copied into the tables.
bool record_table_copies(Renderer* r, VkCommandBuffer cmd) {
    bool ok = true;
    u32 rows = for_dirty_runs(r->sprite_dirty, r->sprites.len, 32, [&](u32 a, u32 b) {
        VkDeviceSize bytes = (VkDeviceSize)(b - a) * sizeof(GpuSprite);
        RingSpan span = rv_ring_alloc(r, bytes, 16);
        if (!span.ptr) { ok = false; return; }
        GpuSprite* dst = (GpuSprite*)span.ptr;
        for (u32 i = a; i < b; ++i) {
            if (r->sprites.is_alive(i)) dst[i - a] = r->sprites.items[i].gpu;
            else dst[i - a] = GpuSprite{}; // a dead slot: not alive, never drawn
        }
        VkBufferCopy region{span.offset, (VkDeviceSize)a * sizeof(GpuSprite), bytes};
        vkCmdCopyBuffer(cmd, span.buffer, r->tables.sprites.buffer, 1, &region);
    });
    r->rows_uploaded += rows;
    KV_COUNTER("sprite rows uploaded", rows);

    for_dirty_runs(r->material_dirty, kMaxMaterials, 8, [&](u32 a, u32 b) {
        VkDeviceSize bytes = (VkDeviceSize)(b - a) * sizeof(GpuMaterial);
        RingSpan span = rv_ring_alloc(r, bytes, 16);
        if (!span.ptr) { ok = false; return; }
        GpuMaterial* dst = (GpuMaterial*)span.ptr;
        for (u32 i = a; i < b; ++i) rv_material_row(r, i, dst[i - a]);
        VkBufferCopy region{span.offset, (VkDeviceSize)a * sizeof(GpuMaterial), bytes};
        vkCmdCopyBuffer(cmd, span.buffer, r->material_table.buffer, 1, &region);
    });
    return ok;
}

// ---- the sort -------------------------------------------------------------------------------

void record_sort(Renderer* r, FrameInFlight& f, VkCommandBuffer cmd) {
    u32 blocks = (r->sprites.len + kBlockRows - 1) / kBlockRows;
    const VkPipelineStageFlags2 cs = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    const VkAccessFlags2 rw = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->pipe_layout, 0, 1, &f.tables, 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->cs_count);
    vkCmdDispatch(cmd, blocks, 1, 1);
    buffer_barrier(cmd, cs, rw, cs, rw);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->cs_scan);
    vkCmdDispatch(cmd, kBucketCount / kComputeGroup, 1, 1);
    buffer_barrier(cmd, cs, rw, cs, rw);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->cs_bucket_scan);
    vkCmdDispatch(cmd, 1, 1, 1);
    buffer_barrier(cmd, cs, rw, cs, rw);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->cs_scatter);
    vkCmdDispatch(cmd, blocks, 1, 1);
    buffer_barrier(cmd, cs, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                   VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}

// ---- the sort check -------------------------------------------------------------------------------

// Copies sorted[], bucket_of[] and the sort meta into a host buffer, after the sort.
void record_sort_check(Renderer* r, VkCommandBuffer cmd) {
    u32 rows = r->sprites.len;
    VkDeviceSize meta_bytes = sizeof(u32) * (2 * kBucketCount + 4);
    VkDeviceSize bytes = sizeof(u32) * (VkDeviceSize)rows * 2 + meta_bytes;
    if (r->check_buffer.buffer) rv_defer_destroy_buffer(r, r->check_buffer);
    if (!rv_create_host_buffer(r, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, r->check_buffer)) { r->check_pending = false; return; }
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
    if (r->shot_buffer.buffer) rv_defer_destroy_buffer(r, r->shot_buffer);
    if (!rv_create_host_buffer(r, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, r->shot_buffer)) { r->shot_pending = false; return; }
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

// One indirect draw per run of buckets: from a non-empty bucket to the last non-empty bucket
// of the same blend before one of another blend intervenes. Buckets in between with nothing
// in them draw zero instances. Without multi-draw, one call per non-empty bucket.
u32 record_draws(Renderer* r, VkCommandBuffer cmd) {
    u32 draws = 0;
    u32 b = 0;
    while (b < kBucketCount) {
        if (!r->bucket_counts[b]) { ++b; continue; }
        u32 blend = b % kBlendCount;
        u32 first = b, last = b;
        for (u32 n = b + 1; n < kBucketCount; ++n) {
            if (!r->bucket_counts[n]) continue;
            if (n % kBlendCount != blend) break;
            last = n;
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipelines[blend]);
        if (r->multi_draw_indirect) {
            vkCmdDrawIndirect(cmd, r->indirect.buffer, (VkDeviceSize)first * sizeof(VkDrawIndirectCommand),
                              last - first + 1, sizeof(VkDrawIndirectCommand));
            ++draws;
        } else {
            for (u32 n = first; n <= last; ++n) {
                if (!r->bucket_counts[n]) continue;
                vkCmdDrawIndirect(cmd, r->indirect.buffer, (VkDeviceSize)n * sizeof(VkDrawIndirectCommand), 1, sizeof(VkDrawIndirectCommand));
                ++draws;
            }
        }
        b = last + 1;
    }
    return draws;
}

} // namespace

// ---- shaders and pipelines --------------------------------------------------------------------

bool rv_load_shaders(Renderer* r) {
    return load_shader(r, "sprite.spv", &r->sprite_shader) && load_shader(r, "sprite_sort.spv", &r->sort_shader);
}

bool rv_create_compute_pipelines(Renderer* r) {
    return create_compute(r, "cs_count", &r->cs_count) && create_compute(r, "cs_scan", &r->cs_scan) &&
           create_compute(r, "cs_bucket_scan", &r->cs_bucket_scan) && create_compute(r, "cs_scatter", &r->cs_scatter);
}

// One graphics pipeline per blend mode; everything else is shared. Needs the swapchain format.
bool rv_create_pipelines(Renderer* r) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = r->sprite_shader;
    stages[0].pName  = "vs_main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = r->sprite_shader;
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

    for (u32 i = 0; i < kBlendCount; ++i) {
        VkPipelineColorBlendAttachmentState attachment = blend_state((BlendMode)i);
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
        VK_CHECK(vkCreateGraphicsPipelines(r->device, VK_NULL_HANDLE, 1, &ci, nullptr, &r->pipelines[i]));
    }
    return true;
}

// A format change: the pipelines in flight retire with the frame.
void rv_defer_destroy_pipelines(Renderer* r) {
    for (u32 i = 0; i < kBlendCount; ++i) {
        rv_defer_destroy(r, Garbage::PipelineKind, (u64)r->pipelines[i]);
        r->pipelines[i] = VK_NULL_HANDLE;
    }
}

void rv_destroy_pipelines_now(Renderer* r) {
    for (u32 i = 0; i < kBlendCount; ++i) {
        if (r->pipelines[i]) vkDestroyPipeline(r->device, r->pipelines[i], nullptr);
        r->pipelines[i] = VK_NULL_HANDLE;
    }
    VkPipeline* cs[] = {&r->cs_count, &r->cs_scan, &r->cs_bucket_scan, &r->cs_scatter};
    for (VkPipeline* p : cs) { if (*p) vkDestroyPipeline(r->device, *p, nullptr); *p = VK_NULL_HANDLE; }
    if (r->sprite_shader) vkDestroyShaderModule(r->device, r->sprite_shader, nullptr);
    if (r->sort_shader)   vkDestroyShaderModule(r->device, r->sort_shader, nullptr);
    r->sprite_shader = r->sort_shader = VK_NULL_HANDLE;
}

// Every bucket's run in sorted[] holds exactly the rows whose bucket it is, in ascending
// row order, once each; the counts agree with the CPU's where nothing was culled.
bool rv_verify_sort(Renderer* r) {
    r->check_recorded = false;
    if (!r->check_buffer.mapped) return false;
    u32 rows = r->check_rows;
    const u32* sorted    = (const u32*)r->check_buffer.mapped;
    const u32* bucket_of = sorted + rows;
    const u32* meta      = bucket_of + rows;
    const u32* totals    = meta;
    const u32* starts    = meta + kBucketCount;
    u32 visible_rows = 0, bad = 0, seen_total = 0;
    MemScope scope;
    u8* seen = arena_push_n<u8>(*scope.arena, rows ? rows : 1);
    memset(seen, 0, rows);
    for (u32 i = 0; i < rows; ++i) if (bucket_of[i] != 0xFFFFFFFFu) ++visible_rows;
    u32 expect_start = 0;
    for (u32 b = 0; b < kBucketCount; ++b) {
        if (starts[b] != expect_start) { ++bad; KV_ERROR("vulkan", "sort check: bucket %u starts at %u, expected %u", b, starts[b], expect_start); }
        expect_start += totals[b];
        if (totals[b] > r->bucket_counts[b]) { ++bad; KV_ERROR("vulkan", "sort check: bucket %u has %u rows on the GPU, %u on the CPU", b, totals[b], r->bucket_counts[b]); }
        u32 prev = 0;
        for (u32 k = 0; k < totals[b]; ++k) {
            u32 row = sorted[starts[b] + k];
            if (row >= rows || bucket_of[row] != b) { if (bad++ < 8) KV_ERROR("vulkan", "sort check: bucket %u entry %u is row %u (bucket %u)", b, k, row, row < rows ? bucket_of[row] : ~0u); continue; }
            if (k && row <= prev) { if (bad++ < 8) KV_ERROR("vulkan", "sort check: bucket %u entry %u row %u after row %u", b, k, row, prev); }
            if (seen[row]++) { if (bad++ < 8) KV_ERROR("vulkan", "sort check: row %u listed twice", row); }
            prev = row;
            ++seen_total;
        }
    }
    if (seen_total != visible_rows || meta[2 * kBucketCount] != visible_rows) {
        ++bad;
        KV_ERROR("vulkan", "sort check: %u rows visible, %u listed, GPU says %u", visible_rows, seen_total, meta[2 * kBucketCount]);
    }
    if (bad) ++r->checks_failed; else ++r->checks_passed;
    KV_INFO("vulkan", "sort check: %u rows, %u visible, %u buckets in use: %s", rows, visible_rows,
            [&] { u32 n = 0; for (u32 b = 0; b < kBucketCount; ++b) n += totals[b] != 0; return n; }(), bad ? "FAILED" : "ok");
    rv_defer_destroy_buffer(r, r->check_buffer);
    return bad == 0;
}

// Binary PPM, RGB. The swapchain is BGRA or RGBA; sRGB-encoded bytes either way.
bool rv_write_screenshot(Renderer* r) {
    r->shot_recorded = false;
    if (!r->shot_buffer.mapped) return false;
    FILE* f = fopen(r->shot_path, "wb");
    if (!f) { KV_ERROR("vulkan", "cannot write screenshot '%s'", r->shot_path); rv_defer_destroy_buffer(r, r->shot_buffer); return false; }
    fprintf(f, "P6\n%u %u\n255\n", r->shot_width, r->shot_height);
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
        fwrite(row, 1, r->shot_width * 3, f);
    }
    fclose(f);
    KV_INFO("vulkan", "screenshot '%s' %ux%u", r->shot_path, r->shot_width, r->shot_height);
    rv_defer_destroy_buffer(r, r->shot_buffer);
    return true;
}

// ---- the frame -----------------------------------------------------------------------------------

bool rv_record_frame(Renderer* r, FrameInFlight& f, bool have_image, u32 image_index, const RenderView& view) {
    KV_ZONE("record");
    Swapchain& s = r->swapchain;
    VkCommandBuffer cmd = f.cmd;

    VK_CHECK(vkResetCommandPool(r->device, f.pool, 0));
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));

    // Uploads first: textures, then the rows the ticks changed. The previous frame read
    // the tables in its compute and vertex stages and wrote them in its copies; both must
    // be done before this frame's copies land.
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

    bool draw = have_image && r->sprites.len > 0;
    affine2 proj = ortho2d(view.view);
    PushData push{};
    push.view[0] = proj.a; push.view[1] = proj.b; push.view[2] = proj.c; push.view[3] = proj.d;
    push.view_trans[0] = proj.tx; push.view_trans[1] = proj.ty;
    push.alpha     = view.alpha;
    push.tick      = r->tick;
    push.cull[0] = view.view.x; push.cull[1] = view.view.y; push.cull[2] = view.view.w; push.cull[3] = view.view.h;
    push.row_count = r->sprites.len;
    vkCmdPushConstants(cmd, r->pipe_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);

    if (draw) {
        record_sort(r, f, cmd);
        if (r->check_pending) record_sort_check(r, cmd);
    }

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
        vkCmdBeginRendering(cmd, &ri);

        if (draw) {
            VkViewport vp{0.0f, 0.0f, (f32)s.extent.width, (f32)s.extent.height, 0.0f, 1.0f};
            VkRect2D scissor{{0, 0}, s.extent};
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            VkDescriptorSet sets[2] = {f.tables, f.textures};
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipe_layout, 0, 2, sets, 0, nullptr);
            u32 draws = record_draws(r, cmd);
            KV_COUNTER("indirect draws", draws);
        }

        vkCmdEndRendering(cmd);

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

    VK_CHECK(vkEndCommandBuffer(cmd));
    return true;
}

} // namespace kv
