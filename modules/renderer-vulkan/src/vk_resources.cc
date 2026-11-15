// Korvid renderer-vulkan: memory, buffers, the frame ring, deferred destruction, textures,
// materials, the handle kinds and pools, the GPU tables and the descriptor sets.
#include "renderer_internal.h"
#include "korvid/core/format.h"
#include <string.h>

namespace kv {

// ---- memory and buffers -------------------------------------------------------------------

u32 rb_find_memory_type(Renderer* r, u32 type_bits, VkMemoryPropertyFlags flags) {
    for (u32 i = 0; i < r->mem_props.memoryTypeCount; ++i)
        if ((type_bits & (1u << i)) && (r->mem_props.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    return UINT32_MAX;
}

namespace {

bool create_buffer(Renderer* r, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, bool map, Buffer& out) {
    VkBufferCreateInfo ci{};
    ci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size        = size;
    ci.usage       = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(r->device, &ci, nullptr, &out.buffer));

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(r->device, out.buffer, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = rb_find_memory_type(r, req.memoryTypeBits, props);
    if (ai.memoryTypeIndex == UINT32_MAX) { KV_ERROR("vulkan", "no memory type with flags 0x%x", props); return false; }
    VK_CHECK(vkAllocateMemory(r->device, &ai, nullptr, &out.memory));
    VK_CHECK(vkBindBufferMemory(r->device, out.buffer, out.memory, 0));
    if (map) VK_CHECK(vkMapMemory(r->device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped));
    out.size = size;
    return true;
}

} // namespace

// Host-visible, coherent, persistently mapped.
bool rb_create_host_buffer(Renderer* r, VkDeviceSize size, VkBufferUsageFlags usage, Buffer& out) {
    return create_buffer(r, size, usage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true, out);
}

bool rb_create_device_buffer(Renderer* r, VkDeviceSize size, VkBufferUsageFlags usage, Buffer& out) {
    return create_buffer(r, size, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, out);
}

void rb_destroy_buffer(Renderer* r, Buffer& b) {
    if (b.mapped) vkUnmapMemory(r->device, b.memory);
    if (b.buffer) vkDestroyBuffer(r->device, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(r->device, b.memory, nullptr);
    b = Buffer{};
}

// ---- deferred destruction -----------------------------------------------------------------
// Queued on the slot of the next frame to be submitted (the one being recorded, or between
// frames the one about to be), run after that frame's fence passes, so nothing the GPU may
// still read is freed early. Fences on one queue pass in submission order, so a frame's
// fence also covers everything earlier frames referenced. What is queued between frames may
// be what the next frame uses (a staging buffer its upload reads) or what the frame still in
// flight reads (a texture replaced meanwhile), so it waits for the next frame's fence, not
// the wait that opens that frame (slice 7 found the staging buffer freed before its copy).

void rb_defer_destroy(Renderer* r, Garbage::Kind kind, u64 handle) {
    if (handle) rc_defer(r, Garbage{kind, handle});
}

void rb_defer_destroy_buffer(Renderer* r, Buffer& b) {
    if (b.mapped) vkUnmapMemory(r->device, b.memory);
    rb_defer_destroy(r, Garbage::BufferKind, (u64)b.buffer);
    rb_defer_destroy(r, Garbage::MemoryKind, (u64)b.memory);
    b = Buffer{};
}

// ---- the frame ring -----------------------------------------------------------------------
// [tail, head) is owned by frames in flight, the frame being recorded included; `used`
// counts those bytes with wrap gaps so head == tail is unambiguous. A frame's allocations
// are contiguous in ring order even when they begin before the previous frame's fence
// passes, so releasing a frame is moving the tail to where that frame ended.

// ---- textures -------------------------------------------------------------------------------

VkFormat rb_vk_format(TextureFormat f) {
    return f == TextureFormat::RGBA8_UNORM ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R8G8B8A8_SRGB;
}

void rb_defer_destroy_texture(Renderer* r, Texture& t) {
    rb_defer_destroy(r, Garbage::ImageViewKind, (u64)t.view);
    rb_defer_destroy(r, Garbage::ImageKind, (u64)t.image);
    rb_defer_destroy(r, Garbage::MemoryKind, (u64)t.memory);
    t.view = VK_NULL_HANDLE; t.image = VK_NULL_HANDLE; t.memory = VK_NULL_HANDLE;
    t.resident = false;
}

void rb_destroy_texture_now(Renderer* r, Texture& t) {
    if (t.view)   vkDestroyImageView(r->device, t.view, nullptr);
    if (t.image)  vkDestroyImage(r->device, t.image, nullptr);
    if (t.memory) vkFreeMemory(r->device, t.memory, nullptr);
    t.view = VK_NULL_HANDLE; t.image = VK_NULL_HANDLE; t.memory = VK_NULL_HANDLE;
    t.resident = false;
}

namespace {

// The view a slot should show right now: its own image, or the default texture's.
VkImageView slot_view(Renderer* r, u32 slot) {
    const Texture* t = slot < r->textures.len && r->textures.is_alive(slot) ? &r->textures.items[slot] : nullptr;
    if (t && t->view) return t->view;
    return r->textures.items[0].view;
}

} // namespace

void rb_write_texture_slot(Renderer* r, VkDescriptorSet set, u32 slot) {
    VkDescriptorImageInfo info{};
    info.imageView   = slot_view(r, slot);
    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = set;
    write.dstBinding      = 1;
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo      = &info;
    vkUpdateDescriptorSets(r->device, 1, &write, 0, nullptr);
}

// The image and view for a texture record's size and format. Any previous image is
// destroyed after the frames in flight.
bool rb_texture_create(Renderer* r, Texture& t) {
    if (t.image) rb_defer_destroy_texture(r, t);

    VkImageCreateInfo ici{};
    ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = rb_vk_format(t.format);
    ici.extent        = {t.width, t.height, 1};
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(r->device, &ici, nullptr, &t.image));
    rb_name(r, VK_OBJECT_TYPE_IMAGE, (u64)t.image, name_str(t.name));

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(r->device, t.image, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = rb_find_memory_type(r, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) { KV_ERROR("vulkan", "no device-local memory type"); return false; }
    VK_CHECK(vkAllocateMemory(r->device, &ai, nullptr, &t.memory));
    VK_CHECK(vkBindImageMemory(r->device, t.image, t.memory, 0));

    VkImageViewCreateInfo vci{};
    vci.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image            = t.image;
    vci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    vci.format           = ici.format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(r->device, &vci, nullptr, &t.view));
    return true;
}

// Copies tightly packed pixels into the frame ring (or a one-off staging buffer when they
// would not fit) and queues the copy for the next frame's command buffer. Resident before
// the next frame that references the slot: the copy is recorded ahead of the draws.
bool rb_stage_texture(Renderer* r, u32 slot, Texture& t, const u8* pixels) {
    VkDeviceSize bytes = (VkDeviceSize)t.width * t.height * 4;
    PendingUpload up{};
    up.slot   = slot;
    up.image  = t.image;
    up.width  = t.width;
    up.height = t.height;
    if (bytes <= r->ring.size / 2) {
        RingSpan span = rc_ring_alloc(r, bytes, 16);
        if (!span.ptr) return false;
        memcpy(span.ptr, pixels, (usize)bytes);
        up.src    = span.buffer.buffer;
        up.offset = span.offset;
    } else {
        Buffer staging;
        if (!rb_create_host_buffer(r, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging)) return false;
        memcpy(staging.mapped, pixels, (usize)bytes);
        up.src    = staging.buffer;
        up.offset = 0;
        rb_defer_destroy_buffer(r, staging); // alive until this frame's fence, after the copy
    }
    r->uploads.push(up);
    t.resident = true;
    return true;
}

// A rectangle of the whole image, its rows packed, into the live image: the copy keeps the
// rest. Only once the image has had its whole upload recorded (resident).
bool rb_stage_texture_region(Renderer* r, u32 slot, Texture& t, const u8* pixels, u32 x, u32 y, u32 w, u32 h) {
    if (!t.resident || !t.image) return false;
    VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
    if (bytes > r->ring.size / 2) return false;
    RingSpan span = rc_ring_alloc(r, bytes, 16);
    if (!span.ptr) return false;
    for (u32 row = 0; row < h; ++row)
        memcpy((u8*)span.ptr + (usize)row * w * 4, pixels + ((usize)(y + row) * t.width + x) * 4, (usize)w * 4);
    PendingUpload up{};
    up.slot   = slot;
    up.image  = t.image;
    up.src    = span.buffer.buffer;
    up.offset = span.offset;
    up.width  = w;
    up.height = h;
    up.x      = x;
    up.y      = y;
    up.keep   = true;
    r->uploads.push(up);
    return true;
}

// ---- the GPU tables ------------------------------------------------------------------------------

// Sprite tables for at least `rows` rows (rounded up to whole blocks). Previous tables
// retire with the frame; every row is marked for upload and every frame's set 0 rewritten.
bool rb_create_tables(Renderer* r, u32 rows) {
    SpriteTables& t = r->tables;
    u32 cap = kBlockRows;
    while (cap < rows) cap *= 2;
    if (cap > kMaxSprites) cap = kMaxSprites;
    if (t.capacity) {
        rb_defer_destroy_buffer(r, t.sprites);
        rb_defer_destroy_buffer(r, t.bucket_of);
        rb_defer_destroy_buffer(r, t.block_hist);
        rb_defer_destroy_buffer(r, t.sorted);
    }
    t.capacity = cap;
    const VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    const VkBufferUsageFlags readable = storage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT; // the sort check reads these back
    if (!rb_create_device_buffer(r, sizeof(GpuSprite) * cap, storage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, t.sprites) ||
        !rb_create_device_buffer(r, sizeof(u32) * cap, readable, t.bucket_of) ||
        !rb_create_device_buffer(r, sizeof(u32) * (VkDeviceSize)t.blocks() * kBucketCount, storage, t.block_hist) ||
        !rb_create_device_buffer(r, sizeof(u32) * cap, readable, t.sorted))
        return false;
    rb_name(r, VK_OBJECT_TYPE_BUFFER, (u64)t.sprites.buffer, "sprite rows");
    rb_name(r, VK_OBJECT_TYPE_BUFFER, (u64)t.bucket_of.buffer, "sprite buckets");
    rb_name(r, VK_OBJECT_TYPE_BUFFER, (u64)t.block_hist.buffer, "sort block counts");
    rb_name(r, VK_OBJECT_TYPE_BUFFER, (u64)t.sorted.buffer, "sorted rows");
    r->world.sprite_dirty.resize_fill(cap / 64, 0);
    for (u32 w = 0; w < (r->world.sprites.len + 63) / 64; ++w) r->world.sprite_dirty[w] = ~(u64)0; // the new table knows nothing
    for (FrameInFlight& f : r->frames) f.tables_dirty = true;
    KV_INFO("vulkan", "sprite tables: %u rows, %u blocks, %llu KB", cap, t.blocks(),
            (unsigned long long)((t.sprites.size + t.bucket_of.size + t.block_hist.size + t.sorted.size) / 1024));
    return true;
}

bool rb_create_static_buffers(Renderer* r) {
    const VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    for (FrameInFlight& f : r->frames) {
        if (!rb_create_host_buffer(r, sizeof(f32) * 4 * kLayerCount, storage, f.layers)) return false;
        memset(f.layers.mapped, 0, sizeof(f32) * 4 * kLayerCount);
        rb_name(r, VK_OBJECT_TYPE_BUFFER, (u64)f.layers.buffer, "layer offsets");
    }
    if (!rb_create_device_buffer(r, sizeof(GpuMaterial) * kMaxMaterials, storage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, r->material_table) ||
        !rb_create_device_buffer(r, sizeof(u32) * (2 * kBucketCount + 4), storage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, r->sort_meta) ||
        !rb_create_device_buffer(r, sizeof(VkDrawIndirectCommand) * kBucketCount, storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, r->indirect))
        return false;
    rb_name(r, VK_OBJECT_TYPE_BUFFER, (u64)r->material_table.buffer, "material rows");
    rb_name(r, VK_OBJECT_TYPE_BUFFER, (u64)r->sort_meta.buffer, "sort totals");
    rb_name(r, VK_OBJECT_TYPE_BUFFER, (u64)r->indirect.buffer, "indirect draws");
    return true;
}

// ---- descriptors ----------------------------------------------------------------------------------
// Set 0, "tables": the sprite rows, materials, sort scratch and indirect commands, read by
// the compute passes and the vertex shader. Set 1, "textures": the sampler and the bindless
// array. Both exist once per frame in flight so a rewrite waits for that frame's fence.
// Binding numbers match sprite_common.slangh; the variable-count array is the last binding
// of its set.

void rb_write_tables_set(Renderer* r, FrameInFlight& f) {
    VkDescriptorBufferInfo infos[9] = {
        {r->tables.sprites.buffer, 0, VK_WHOLE_SIZE},
        {r->material_table.buffer, 0, VK_WHOLE_SIZE},
        {r->tables.sorted.buffer, 0, VK_WHOLE_SIZE},
        {r->tables.bucket_of.buffer, 0, VK_WHOLE_SIZE},
        {r->tables.block_hist.buffer, 0, VK_WHOLE_SIZE},
        {r->sort_meta.buffer, 0, VK_WHOLE_SIZE},
        {r->indirect.buffer, 0, VK_WHOLE_SIZE},
        {f.layers.buffer, 0, VK_WHOLE_SIZE},
        // The UI lists; until a slot has its own buffer, a stand-in that is never read.
        {f.ui.buffer ? f.ui.buffer : f.layers.buffer, 0, VK_WHOLE_SIZE},
    };
    VkWriteDescriptorSet writes[9]{};
    for (u32 i = 0; i < 9; ++i) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = f.tables;
        writes[i].dstBinding      = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo     = &infos[i];
    }
    vkUpdateDescriptorSets(r->device, 9, writes, 0, nullptr);
    f.tables_dirty = false;
}

bool rb_create_descriptors(Renderer* r) {
    // Set 0.
    VkDescriptorSetLayoutBinding tb[9]{};
    for (u32 i = 0; i < 9; ++i) {
        tb[i].binding         = i;
        tb[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        tb[i].descriptorCount = 1;
        tb[i].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT; // a game's fragment shader reads the materials
    }
    VkDescriptorSetLayoutCreateInfo tlci{};
    tlci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    tlci.bindingCount = 9;
    tlci.pBindings    = tb;
    VK_CHECK(vkCreateDescriptorSetLayout(r->device, &tlci, nullptr, &r->tables_layout));

    // Set 1.
    VkDescriptorSetLayoutBinding xb[2]{};
    xb[0].binding         = 0; // samplers: nearest, linear
    xb[0].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
    xb[0].descriptorCount = 2;
    xb[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    xb[1].binding         = 1; // bindless textures
    xb[1].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    xb[1].descriptorCount = kMaxTextures;
    xb[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorBindingFlags xflags[2] = {
        0,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
            VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT,
    };
    VkDescriptorSetLayoutBindingFlagsCreateInfo xfci{};
    xfci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    xfci.bindingCount  = 2;
    xfci.pBindingFlags = xflags;
    VkDescriptorSetLayoutCreateInfo xlci{};
    xlci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    xlci.pNext        = &xfci;
    xlci.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    xlci.bindingCount = 2;
    xlci.pBindings    = xb;
    VK_CHECK(vkCreateDescriptorSetLayout(r->device, &xlci, nullptr, &r->textures_layout));

    VkDescriptorPoolSize sizes[3] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 9 * kFramesInFlight},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 2 * kFramesInFlight},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxTextures * kFramesInFlight},
    };
    VkDescriptorPoolCreateInfo pci{};
    pci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.flags         = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pci.maxSets       = 2 * kFramesInFlight;
    pci.poolSizeCount = 3;
    pci.pPoolSizes    = sizes;
    VK_CHECK(vkCreateDescriptorPool(r->device, &pci, nullptr, &r->desc_pool));

    VkDescriptorSetLayout tlayouts[kFramesInFlight], xlayouts[kFramesInFlight];
    u32 variable_counts[kFramesInFlight];
    VkDescriptorSet tsets[kFramesInFlight], xsets[kFramesInFlight];
    for (u32 i = 0; i < kFramesInFlight; ++i) { tlayouts[i] = r->tables_layout; xlayouts[i] = r->textures_layout; variable_counts[i] = kMaxTextures; }
    VkDescriptorSetAllocateInfo tai{};
    tai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    tai.descriptorPool     = r->desc_pool;
    tai.descriptorSetCount = kFramesInFlight;
    tai.pSetLayouts        = tlayouts;
    VK_CHECK(vkAllocateDescriptorSets(r->device, &tai, tsets));
    VkDescriptorSetVariableDescriptorCountAllocateInfo vai{};
    vai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
    vai.descriptorSetCount = kFramesInFlight;
    vai.pDescriptorCounts  = variable_counts;
    VkDescriptorSetAllocateInfo xai{};
    xai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    xai.pNext              = &vai;
    xai.descriptorPool     = r->desc_pool;
    xai.descriptorSetCount = kFramesInFlight;
    xai.pSetLayouts        = xlayouts;
    VK_CHECK(vkAllocateDescriptorSets(r->device, &xai, xsets));
    for (u32 i = 0; i < kFramesInFlight; ++i) { r->frames[i].tables = tsets[i]; r->frames[i].textures = xsets[i]; }

    VkSamplerCreateInfo sci{};
    sci.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter    = VK_FILTER_NEAREST;
    sci.minFilter    = VK_FILTER_NEAREST;
    sci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT; // uv outside [0, 1] tiles
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod       = 1.0f;
    VK_CHECK(vkCreateSampler(r->device, &sci, nullptr, &r->sampler));
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    VK_CHECK(vkCreateSampler(r->device, &sci, nullptr, &r->sampler_linear));

    VkDescriptorImageInfo sinfo[2]{};
    sinfo[0].sampler = r->sampler;
    sinfo[1].sampler = r->sampler_linear;
    VkWriteDescriptorSet writes[kFramesInFlight]{};
    for (u32 i = 0; i < kFramesInFlight; ++i) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = r->frames[i].textures;
        writes[i].dstBinding      = 0;
        writes[i].descriptorCount = 2;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[i].pImageInfo      = sinfo;
    }
    vkUpdateDescriptorSets(r->device, kFramesInFlight, writes, 0, nullptr);
    for (u32 i = 0; i < kFramesInFlight; ++i) rb_write_tables_set(r, r->frames[i]);

    VkDescriptorSetLayout layouts[2] = {r->tables_layout, r->textures_layout};
    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushData)};
    VkPipelineLayoutCreateInfo plci{};
    plci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount         = 2;
    plci.pSetLayouts            = layouts;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges    = &range;
    VK_CHECK(vkCreatePipelineLayout(r->device, &plci, nullptr, &r->pipe_layout));
    return true;
}

// Device idle: textures, tables, ring, descriptors, pools. Pipelines, frames and the
// swapchain are the other files' to destroy.
void rb_destroy_resources(Renderer* r) {
    for (u32 i = 0; i < r->textures.len; ++i)
        if (r->textures.is_alive(i)) rb_destroy_texture_now(r, r->textures.items[i]);
    rb_destroy_buffer(r, r->tables.sprites);
    rb_destroy_buffer(r, r->tables.bucket_of);
    rb_destroy_buffer(r, r->tables.block_hist);
    rb_destroy_buffer(r, r->tables.sorted);
    rb_destroy_buffer(r, r->material_table);
    rb_destroy_buffer(r, r->sort_meta);
    rb_destroy_buffer(r, r->indirect);
    rb_destroy_buffer(r, r->ring.buf);
    rb_destroy_buffer(r, r->shot_buffer);
    rb_destroy_buffer(r, r->check_buffer);
    if (r->pipe_layout)     vkDestroyPipelineLayout(r->device, r->pipe_layout, nullptr);
    if (r->sampler)         vkDestroySampler(r->device, r->sampler, nullptr);
    if (r->sampler_linear)  vkDestroySampler(r->device, r->sampler_linear, nullptr);
    if (r->desc_pool)       vkDestroyDescriptorPool(r->device, r->desc_pool, nullptr); // frees the sets
    if (r->tables_layout)   vkDestroyDescriptorSetLayout(r->device, r->tables_layout, nullptr);
    if (r->textures_layout) vkDestroyDescriptorSetLayout(r->device, r->textures_layout, nullptr);
    r->pipe_layout = VK_NULL_HANDLE; r->sampler = VK_NULL_HANDLE; r->sampler_linear = VK_NULL_HANDLE; r->desc_pool = VK_NULL_HANDLE;
    r->tables_layout = VK_NULL_HANDLE; r->textures_layout = VK_NULL_HANDLE;
    rc_free_pools(r);
    r->uploads.free();
}

// ---- what the render core asks of a buffer and of the garbage queue (core.h) ----------------------

void* rb_buffer_ptr(const RbBuffer& b) { return b.mapped; }
void  rb_buffer_retire(Renderer* r, RbBuffer& b) { rb_defer_destroy_buffer(r, b); }

bool rb_ring_buffer(Renderer* r, u64 bytes, RbBuffer& out) {
    if (!rb_create_host_buffer(r, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, out)) return false;
    rb_name(r, VK_OBJECT_TYPE_BUFFER, (u64)out.buffer, "upload ring");
    return true;
}

void rb_garbage_destroy(Renderer* r, const RbGarbage& g) {
    switch (g.kind) {
    case Garbage::BufferKind:    vkDestroyBuffer(r->device, (VkBuffer)g.handle, nullptr); break;
    case Garbage::ImageKind:     vkDestroyImage(r->device, (VkImage)g.handle, nullptr); break;
    case Garbage::ImageViewKind: vkDestroyImageView(r->device, (VkImageView)g.handle, nullptr); break;
    case Garbage::MemoryKind:    vkFreeMemory(r->device, (VkDeviceMemory)g.handle, nullptr); break;
    case Garbage::SwapchainKind: vkDestroySwapchainKHR(r->device, (VkSwapchainKHR)g.handle, nullptr); break;
    case Garbage::SemaphoreKind: vkDestroySemaphore(r->device, (VkSemaphore)g.handle, nullptr); break;
    case Garbage::PipelineKind:  vkDestroyPipeline(r->device, (VkPipeline)g.handle, nullptr); break;
    }
}

} // namespace kv
