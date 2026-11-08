// Korvid renderer-vulkan: memory, buffers, the frame ring, deferred destruction, textures,
// materials, the handle kinds and pools, the GPU tables and the descriptor sets.
#include "renderer_internal.h"
#include <stdio.h>
#include <string.h>

namespace kv {

// ---- memory and buffers -------------------------------------------------------------------

u32 rv_find_memory_type(Renderer* r, u32 type_bits, VkMemoryPropertyFlags flags) {
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
    ai.memoryTypeIndex = rv_find_memory_type(r, req.memoryTypeBits, props);
    if (ai.memoryTypeIndex == UINT32_MAX) { KV_ERROR("vulkan", "no memory type with flags 0x%x", props); return false; }
    VK_CHECK(vkAllocateMemory(r->device, &ai, nullptr, &out.memory));
    VK_CHECK(vkBindBufferMemory(r->device, out.buffer, out.memory, 0));
    if (map) VK_CHECK(vkMapMemory(r->device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped));
    out.size = size;
    return true;
}

} // namespace

// Host-visible, coherent, persistently mapped.
bool rv_create_host_buffer(Renderer* r, VkDeviceSize size, VkBufferUsageFlags usage, Buffer& out) {
    return create_buffer(r, size, usage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true, out);
}

bool rv_create_device_buffer(Renderer* r, VkDeviceSize size, VkBufferUsageFlags usage, Buffer& out) {
    return create_buffer(r, size, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, out);
}

void rv_destroy_buffer(Renderer* r, Buffer& b) {
    if (b.mapped) vkUnmapMemory(r->device, b.memory);
    if (b.buffer) vkDestroyBuffer(r->device, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(r->device, b.memory, nullptr);
    b = Buffer{};
}

// ---- deferred destruction -----------------------------------------------------------------
// Queued on the frame being recorded, run after its fence passes, so nothing the GPU may
// still read is freed early. Fences on one queue pass in submission order, so a frame's
// fence also covers everything earlier frames referenced.

void rv_defer_destroy(Renderer* r, Garbage::Kind kind, u64 handle) {
    if (handle) r->frames[r->frame_index].garbage.push(Garbage{kind, handle});
}

void rv_defer_destroy_buffer(Renderer* r, Buffer& b) {
    if (b.mapped) vkUnmapMemory(r->device, b.memory);
    rv_defer_destroy(r, Garbage::BufferKind, (u64)b.buffer);
    rv_defer_destroy(r, Garbage::MemoryKind, (u64)b.memory);
    b = Buffer{};
}

void rv_run_garbage(Renderer* r, FrameInFlight& f) {
    for (const Garbage& g : f.garbage) {
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
    f.garbage.clear();
}

// ---- the frame ring -----------------------------------------------------------------------
// [tail, head) is owned by frames in flight, the frame being recorded included; `used`
// counts those bytes with wrap gaps so head == tail is unambiguous. A frame's allocations
// are contiguous in ring order even when they begin before the previous frame's fence
// passes, so releasing a frame is moving the tail to where that frame ended.

bool rv_ring_init(Renderer* r, VkDeviceSize bytes) {
    FrameRing& g = r->ring;
    if (!rv_create_host_buffer(r, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, g.buf)) return false;
    g.head = g.tail = g.used = g.frame_bytes = 0;
    return true;
}

namespace {

bool ring_grow(Renderer* r, VkDeviceSize need) {
    FrameRing& g = r->ring;
    VkDeviceSize cap = g.buf.size * 2;
    while (cap < need * 2) cap *= 2;
    // Spans handed out so far point at the old buffer, which stays alive until this
    // frame's fence; the accounting restarts in the new one.
    rv_defer_destroy_buffer(r, g.buf);
    if (!rv_create_host_buffer(r, cap, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, g.buf)) return false;
    g.head = g.tail = g.used = 0;
    for (FrameInFlight& f : r->frames) f.ring_bytes = 0; // the old buffer's spans are the garbage queue's now
    ++g.grows;
    KV_INFO("vulkan", "frame ring grown to %llu KB", (unsigned long long)(cap / 1024));
    return true;
}

} // namespace

RingSpan rv_ring_alloc(Renderer* r, VkDeviceSize bytes, VkDeviceSize align) {
    FrameRing& g = r->ring;
    RingSpan out;
    if (bytes == 0) return out;
    for (u32 attempt = 0; attempt < 2; ++attempt) {
        VkDeviceSize cap  = g.buf.size;
        VkDeviceSize head = (g.head + align - 1) / align * align;
        VkDeviceSize gap  = head - g.head;
        bool ok = false;
        if (g.used == 0) {
            g.head = g.tail = 0; head = 0; gap = 0;
            ok = bytes <= cap;
        } else if (g.head >= g.tail) {         // free: [head, cap) then [0, tail)
            if (head + bytes <= cap) ok = true;
            else if (bytes < g.tail) { gap = cap - g.head; head = 0; ok = true; } // wrap; the rest of the buffer is a gap
        } else {                               // free: [head, tail)
            ok = head + bytes < g.tail;
        }
        if (ok) {
            out.buffer = g.buf.buffer;
            out.offset = head;
            out.ptr    = (u8*)g.buf.mapped + head;
            g.head     = head + bytes;
            if (g.head == cap) g.head = 0;
            g.used        += gap + bytes;
            g.frame_bytes += gap + bytes;
            if (g.used > g.peak) g.peak = g.used;
            return out;
        }
        if (!ring_grow(r, bytes + align)) return RingSpan{};
    }
    return out;
}

void rv_ring_submitted(Renderer* r, FrameInFlight& f) {
    f.ring_end   = r->ring.head;
    f.ring_bytes = r->ring.frame_bytes;
    r->ring.frame_bytes = 0;
}

void rv_ring_release(Renderer* r, FrameInFlight& f) {
    FrameRing& g = r->ring;
    // A frame that took nothing (or whose spans went with a retired buffer) ends where
    // the frame before it did; its end mark says nothing about the current buffer.
    if (!f.submitted || f.ring_bytes == 0) return;
    g.used -= f.ring_bytes;
    g.tail  = f.ring_end;
    f.ring_bytes = 0;
    if (g.used == 0) g.head = g.tail = 0;
}

// ---- textures -------------------------------------------------------------------------------

VkFormat rv_vk_format(TextureFormat f) {
    return f == TextureFormat::RGBA8_UNORM ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R8G8B8A8_SRGB;
}

void rv_defer_destroy_texture(Renderer* r, Texture& t) {
    rv_defer_destroy(r, Garbage::ImageViewKind, (u64)t.view);
    rv_defer_destroy(r, Garbage::ImageKind, (u64)t.image);
    rv_defer_destroy(r, Garbage::MemoryKind, (u64)t.memory);
    t.view = VK_NULL_HANDLE; t.image = VK_NULL_HANDLE; t.memory = VK_NULL_HANDLE;
    t.resident = false;
}

void rv_destroy_texture_now(Renderer* r, Texture& t) {
    if (t.view)   vkDestroyImageView(r->device, t.view, nullptr);
    if (t.image)  vkDestroyImage(r->device, t.image, nullptr);
    if (t.memory) vkFreeMemory(r->device, t.memory, nullptr);
    t.view = VK_NULL_HANDLE; t.image = VK_NULL_HANDLE; t.memory = VK_NULL_HANDLE;
    t.resident = false;
}

// A slot's contents changed. Each frame's set is rewritten once that frame's fence has
// passed, so no pending command buffer sees a descriptor change under it.
void rv_mark_slot_dirty(Renderer* r, u32 slot) {
    for (FrameInFlight& f : r->frames) f.dirty_slots.push(slot);
}

namespace {

// The view a slot should show right now: its own image, or the default texture's.
VkImageView slot_view(Renderer* r, u32 slot) {
    const Texture* t = slot < r->textures.len && r->textures.is_alive(slot) ? &r->textures.items[slot] : nullptr;
    if (t && t->view) return t->view;
    return r->textures.items[0].view;
}

} // namespace

void rv_write_texture_slot(Renderer* r, VkDescriptorSet set, u32 slot) {
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
bool rv_create_texture_image(Renderer* r, Texture& t) {
    if (t.image) rv_defer_destroy_texture(r, t);

    VkImageCreateInfo ici{};
    ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = rv_vk_format(t.format);
    ici.extent        = {t.width, t.height, 1};
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(r->device, &ici, nullptr, &t.image));

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(r->device, t.image, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = rv_find_memory_type(r, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
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
bool rv_stage_texture(Renderer* r, u32 slot, Texture& t, const u8* pixels) {
    VkDeviceSize bytes = (VkDeviceSize)t.width * t.height * 4;
    PendingUpload up{};
    up.slot   = slot;
    up.image  = t.image;
    up.width  = t.width;
    up.height = t.height;
    if (bytes <= r->ring.buf.size / 2) {
        RingSpan span = rv_ring_alloc(r, bytes, 16);
        if (!span.ptr) return false;
        memcpy(span.ptr, pixels, (usize)bytes);
        up.src    = span.buffer;
        up.offset = span.offset;
    } else {
        Buffer staging;
        if (!rv_create_host_buffer(r, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging)) return false;
        memcpy(staging.mapped, pixels, (usize)bytes);
        up.src    = staging.buffer;
        up.offset = 0;
        rv_defer_destroy_buffer(r, staging); // alive until this frame's fence, after the copy
    }
    r->uploads.push(up);
    t.resident = true;
    return true;
}

// The hardcoded default: a red and white checkerboard at slot 0; what a stale or null
// texture reference shows, and what a pending texture shows until its upload. Its upload
// rides the first frame like any other; until then every slot points at its view.
bool rv_create_default_texture(Renderer* r) {
    Texture t{};
    t.width = t.height = kDefaultTexSize;
    t.format = TextureFormat::RGBA8_SRGB;
    strncpy(t.name, "default", sizeof t.name - 1);
    Handle h = r->textures.create(t);
    if (h.index() != 0) { KV_ERROR("vulkan", "default texture is not slot 0"); return false; }
    Texture& slot0 = r->textures.items[0];
    if (!rv_create_texture_image(r, slot0)) return false;

    u8 pixels[kDefaultTexSize * kDefaultTexSize * 4];
    for (u32 y = 0; y < kDefaultTexSize; ++y) {
        for (u32 x = 0; x < kDefaultTexSize; ++x) {
            bool red = ((x / kDefaultTexCell) + (y / kDefaultTexCell)) & 1;
            u8* p = pixels + (y * kDefaultTexSize + x) * 4;
            p[0] = 255;
            p[1] = red ? 0 : 255;
            p[2] = red ? 0 : 255;
            p[3] = 255;
        }
    }
    if (!rv_stage_texture(r, 0, slot0, pixels)) return false;
    for (FrameInFlight& f : r->frames)
        for (u32 slot = 0; slot < kMaxTextures; ++slot) rv_write_texture_slot(r, f.textures, slot);

    MaterialRecord m{};
    m.texture = h;
    m.blend   = BlendMode::Alpha;
    m.tint    = color_white();
    r->default_material = r->materials.create(m);
    if (r->default_material.index() != 0) { KV_ERROR("vulkan", "default material is not row 0"); return false; }
    rv_mark_material_dirty(r, 0);
    return true;
}

// ---- materials --------------------------------------------------------------------------------

void rv_material_row(Renderer* r, u32 row, GpuMaterial& out) {
    out = GpuMaterial{};
    const MaterialRecord* m = r->materials.is_alive(row) ? &r->materials.items[row] : nullptr;
    if (!m) m = &r->materials.items[0];
    out.tint[0] = m->tint.r; out.tint[1] = m->tint.g; out.tint[2] = m->tint.b; out.tint[3] = m->tint.a;
    for (u32 i = 0; i < KV_MATERIAL_PARAMS; ++i) out.params[i] = m->params[i];
    out.texture = renderer_texture_slot(r, m->texture);
    out.blend   = (u32)m->blend;
    out.filter  = (u32)m->filter;
}

void rv_mark_material_dirty(Renderer* r, u32 row) {
    r->material_dirty[row >> 6] |= (u64)1 << (row & 63);
}

void rv_rebind_material(Renderer* r, u32 row) {
    u32 blend = r->materials.is_alive(row) ? (u32)r->materials.items[row].blend : (u32)r->materials.items[0].blend;
    for (u32 i = 0; i < r->sprites.len; ++i) {
        if (!r->sprites.is_alive(i)) continue;
        RenderSprite& s = r->sprites.items[i];
        if (s.gpu.material != row) continue;
        if (!r->materials.is_alive(row)) s.gpu.material = 0; // the material is gone: the default
        u32 bucket = rv_bucket_of(s.gpu.layer, (BlendMode)blend);
        if (bucket != s.bucket) {
            --r->bucket_counts[s.bucket];
            ++r->bucket_counts[bucket];
            if (s.keyed) { --r->keyed_counts[s.bucket]; ++r->keyed_counts[bucket]; }
            s.bucket = bucket;
        }
        r->sprite_dirty[i >> 6] |= (u64)1 << (i & 63);
    }
}

// ---- handle kinds and pools -----------------------------------------------------------------

namespace {

void* texture_resolve(kv_handle h, void* ctx) { return ((Renderer*)ctx)->textures.get(Handle{h}); }
int texture_describe(kv_handle h, void* ctx, char* out, size_t cap) {
    Renderer* r = (Renderer*)ctx;
    Texture* t = r->textures.get(Handle{h});
    if (!t) return snprintf(out, cap, "stale");
    return snprintf(out, cap, "'%s' %ux%u slot %u %s", t->name, t->width, t->height, Handle{h}.index(),
                    t->resident ? "resident" : "placeholder");
}
void* material_resolve(kv_handle h, void* ctx) { return ((Renderer*)ctx)->materials.get(Handle{h}); }
int material_describe(kv_handle h, void* ctx, char* out, size_t cap) {
    Renderer* r = (Renderer*)ctx;
    MaterialRecord* m = r->materials.get(Handle{h});
    if (!m) return snprintf(out, cap, "stale");
    return snprintf(out, cap, "%s texture slot %u row %u", blend_mode_name(m->blend), renderer_texture_slot(r, m->texture), Handle{h}.index());
}

} // namespace

bool rv_create_resource_pools(Renderer* r) {
    kv_handle_kind_desc td{};
    td.name = "render.texture"; td.type_key = type_key<Texture>(); td.ctx = r;
    td.resolve = texture_resolve; td.describe = texture_describe; td.flags = KV_HANDLE_GENERATIONAL;
    r->texture_kind = handle_kind_register(td);
    kv_handle_kind_desc md{};
    md.name = "render.material"; md.type_key = type_key<MaterialRecord>(); md.ctx = r;
    md.resolve = material_resolve; md.describe = material_describe; md.flags = KV_HANDLE_GENERATIONAL;
    r->material_kind = handle_kind_register(md);
    if (!r->texture_kind || !r->material_kind) return false;
    r->textures.use_kind(r->texture_kind);
    r->materials.use_kind(r->material_kind);
    r->sprite_of_entity.init(r->general);
    r->sprite_dirty.alloc = r->material_dirty.alloc = r->uploads.alloc = r->general;
    r->material_dirty.resize_fill((kMaxMaterials + 63) / 64, 0);
    return r->textures.init(kMaxTextures, r->general->tag) && r->materials.init(kMaxMaterials, r->general->tag) &&
           r->sprites.init(kMaxSprites, r->general->tag);
}

// ---- the GPU tables ------------------------------------------------------------------------------

// Sprite tables for at least `rows` rows (rounded up to whole blocks). Previous tables
// retire with the frame; every row is marked for upload and every frame's set 0 rewritten.
bool rv_create_tables(Renderer* r, u32 rows) {
    SpriteTables& t = r->tables;
    u32 cap = kBlockRows;
    while (cap < rows) cap *= 2;
    if (cap > kMaxSprites) cap = kMaxSprites;
    if (t.capacity) {
        rv_defer_destroy_buffer(r, t.sprites);
        rv_defer_destroy_buffer(r, t.bucket_of);
        rv_defer_destroy_buffer(r, t.block_hist);
        rv_defer_destroy_buffer(r, t.sorted);
    }
    t.capacity = cap;
    const VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    const VkBufferUsageFlags readable = storage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT; // the sort check reads these back
    if (!rv_create_device_buffer(r, sizeof(GpuSprite) * cap, storage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, t.sprites) ||
        !rv_create_device_buffer(r, sizeof(u32) * cap, readable, t.bucket_of) ||
        !rv_create_device_buffer(r, sizeof(u32) * (VkDeviceSize)t.blocks() * kBucketCount, storage, t.block_hist) ||
        !rv_create_device_buffer(r, sizeof(u32) * cap, readable, t.sorted))
        return false;
    r->sprite_dirty.resize_fill(cap / 64, 0);
    for (u32 w = 0; w < (r->sprites.len + 63) / 64; ++w) r->sprite_dirty[w] = ~(u64)0; // the new table knows nothing
    for (FrameInFlight& f : r->frames) f.tables_dirty = true;
    KV_INFO("vulkan", "sprite tables: %u rows, %u blocks, %llu KB", cap, t.blocks(),
            (unsigned long long)((t.sprites.size + t.bucket_of.size + t.block_hist.size + t.sorted.size) / 1024));
    return true;
}

bool rv_create_static_buffers(Renderer* r) {
    const VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    return rv_create_device_buffer(r, sizeof(GpuMaterial) * kMaxMaterials, storage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, r->material_table) &&
           rv_create_device_buffer(r, sizeof(u32) * (2 * kBucketCount + 4), storage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, r->sort_meta) &&
           rv_create_device_buffer(r, sizeof(VkDrawIndirectCommand) * kBucketCount, storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, r->indirect);
}

// ---- descriptors ----------------------------------------------------------------------------------
// Set 0, "tables": the sprite rows, materials, sort scratch and indirect commands, read by
// the compute passes and the vertex shader. Set 1, "textures": the sampler and the bindless
// array. Both exist once per frame in flight so a rewrite waits for that frame's fence.
// Binding numbers match sprite_common.slangh; the variable-count array is the last binding
// of its set.

void rv_write_tables_set(Renderer* r, FrameInFlight& f) {
    VkDescriptorBufferInfo infos[7] = {
        {r->tables.sprites.buffer, 0, VK_WHOLE_SIZE},
        {r->material_table.buffer, 0, VK_WHOLE_SIZE},
        {r->tables.sorted.buffer, 0, VK_WHOLE_SIZE},
        {r->tables.bucket_of.buffer, 0, VK_WHOLE_SIZE},
        {r->tables.block_hist.buffer, 0, VK_WHOLE_SIZE},
        {r->sort_meta.buffer, 0, VK_WHOLE_SIZE},
        {r->indirect.buffer, 0, VK_WHOLE_SIZE},
    };
    VkWriteDescriptorSet writes[7]{};
    for (u32 i = 0; i < 7; ++i) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = f.tables;
        writes[i].dstBinding      = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo     = &infos[i];
    }
    vkUpdateDescriptorSets(r->device, 7, writes, 0, nullptr);
    f.tables_dirty = false;
}

bool rv_create_descriptors(Renderer* r) {
    // Set 0.
    VkDescriptorSetLayoutBinding tb[7]{};
    for (u32 i = 0; i < 7; ++i) {
        tb[i].binding         = i;
        tb[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        tb[i].descriptorCount = 1;
        tb[i].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo tlci{};
    tlci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    tlci.bindingCount = 7;
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
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 7 * kFramesInFlight},
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
    for (u32 i = 0; i < kFramesInFlight; ++i) rv_write_tables_set(r, r->frames[i]);

    VkDescriptorSetLayout layouts[2] = {r->tables_layout, r->textures_layout};
    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushData)};
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
void rv_destroy_resources(Renderer* r) {
    for (u32 i = 0; i < r->textures.len; ++i)
        if (r->textures.is_alive(i)) rv_destroy_texture_now(r, r->textures.items[i]);
    rv_destroy_buffer(r, r->tables.sprites);
    rv_destroy_buffer(r, r->tables.bucket_of);
    rv_destroy_buffer(r, r->tables.block_hist);
    rv_destroy_buffer(r, r->tables.sorted);
    rv_destroy_buffer(r, r->material_table);
    rv_destroy_buffer(r, r->sort_meta);
    rv_destroy_buffer(r, r->indirect);
    rv_destroy_buffer(r, r->ring.buf);
    rv_destroy_buffer(r, r->shot_buffer);
    rv_destroy_buffer(r, r->check_buffer);
    if (r->pipe_layout)     vkDestroyPipelineLayout(r->device, r->pipe_layout, nullptr);
    if (r->sampler)         vkDestroySampler(r->device, r->sampler, nullptr);
    if (r->sampler_linear)  vkDestroySampler(r->device, r->sampler_linear, nullptr);
    if (r->desc_pool)       vkDestroyDescriptorPool(r->device, r->desc_pool, nullptr); // frees the sets
    if (r->tables_layout)   vkDestroyDescriptorSetLayout(r->device, r->tables_layout, nullptr);
    if (r->textures_layout) vkDestroyDescriptorSetLayout(r->device, r->textures_layout, nullptr);
    r->pipe_layout = VK_NULL_HANDLE; r->sampler = VK_NULL_HANDLE; r->sampler_linear = VK_NULL_HANDLE; r->desc_pool = VK_NULL_HANDLE;
    r->tables_layout = VK_NULL_HANDLE; r->textures_layout = VK_NULL_HANDLE;
    if (r->texture_kind)  kv_handle_kind_unregister(r->texture_kind);
    if (r->material_kind) kv_handle_kind_unregister(r->material_kind);
    r->texture_kind = r->material_kind = 0;
    r->textures.free();
    r->materials.free();
    r->sprites.free();
    r->sprite_of_entity.free();
    r->sprite_dirty.free();
    r->material_dirty.free();
    r->uploads.free();
}

} // namespace kv
