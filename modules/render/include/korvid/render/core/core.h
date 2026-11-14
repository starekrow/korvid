// Korvid render core: the half of the renderer that is compiled into each backend, and the
// contract between the two (docs/PRDs/Engine-Slice-9-PRD.md). The core shares structure, it
// does not hide the backends: it is built inside the bound backend's library, after that
// backend's renderer_internal.h, and sees its types whole.
//
// How a backend takes part. Its src/render_core.cc (.mm for Metal) is two lines:
//
//     #include "renderer_internal.h"
//     #include "korvid/render/core/core.inl"
//
// Its renderer_internal.h, before it includes this header, defines four values:
//
//     KV_RB_NAME         "vulkan": the log channel and RenderCaps::backend
//     KV_RB_CLIP_Y_UP    0 or 1: whether clip space has y up, so the core flips the
//                        projections it pushes (ortho2d targets Vulkan's, y down)
//     KV_RB_SHADER_EXT   ".spv": what follows a compiled shader's name in <exe>/shaders/
//     KV_RB_SURFACE      the RendererSurface the backend draws into
//
// and two types, each whatever the backend likes:
//
//     RbBuffer           a GPU buffer and, where it has one, its mapping (a struct, a pointer)
//     RbGarbage          one thing to destroy once the frame that last used it is done
//
// The core uses these for what they are and never asks which backend it is: there is no
// `#if` on a backend's name in the core.
//
// After this header the backend completes the records the core began, by deriving from
// them: plain data on plain data, no virtual anything. The core's functions use the core's
// part; the backend's files see one struct.
//
//     struct Texture       : TextureCore   { the image }
//     struct GamePipe      : GamePipeCore  { the module, the pipeline }
//     struct PostPass      : PostCore      { the target, the pipeline }
//     struct FrameInFlight : FrameCore     { the command buffer, the fence, the sets }
//     struct Renderer      : RenderCore    { the device and everything made from it }
//
// A backend's Renderer has, by these names, what the core's functions reach through it:
//
//     Pool<Texture> textures;                  slot = index; 0 is the default checkerboard
//     GamePipe      game_pipes[kGameSlots];
//     PostPass      post;
//     FrameInFlight frames[kFramesInFlight];
//     GpuProfiler   gpu_prof;                  with `RendererGpuStats last`
//
// and it defines the rb_ functions declared at the end of this header.
//
// The rules of the seam:
//   1. No templates over the backend, no virtual functions, no tables of function pointers.
//      Plain structs and plain functions: a name has one definition in a build.
//   2. A function is the core's when its bodies in the backends are the same once the names
//      are normalised. If they are only alike, it stays in the backends.
//   3. The core does not reach into a backend's own state. The backends read and write the
//      core's fields freely: no accessors, no context structs.
//   4. The contract is at the height of a pass ("copy these rows", "draw this run of
//      buckets"), never of an API call.
//   5. What needs no GPU type is not here but in render's linked half (korvid/render/world.h),
//      where it has tests that run without a GPU. A piece that has to be argued into either
//      half is in the wrong one.
//
// Names: rw_ is the linked half's, rc_ this half's, rb_ a backend's.
#pragma once
#include "korvid/render/core/layout.h"
#include "korvid/render/world.h"

#ifndef KV_RB_NAME
#error "the backend's renderer_internal.h defines KV_RB_NAME before including korvid/render/core/core.h"
#endif
#ifndef KV_RB_CLIP_Y_UP
#error "the backend's renderer_internal.h defines KV_RB_CLIP_Y_UP (0 or 1)"
#endif
#ifndef KV_RB_SHADER_EXT
#error "the backend's renderer_internal.h defines KV_RB_SHADER_EXT"
#endif
#ifndef KV_RB_SURFACE
#error "the backend's renderer_internal.h defines KV_RB_SURFACE"
#endif

namespace kv {

struct Renderer;
struct Texture;
struct GamePipe;
struct FrameInFlight;

// ---- the records the backends complete -------------------------------------------------------

// A bindless slot. Its pool index is the slot number. No image yet means the slot shows
// the default texture.
struct TextureCore {
    u32           width  = 0, height = 0;
    TextureFormat format = TextureFormat::RGBA8_SRGB;
    bool          resident = false;
    Name          name;   // TextureDesc::name, or "texture:<n>"
};

// A game fragment shader with a blend: a pipeline slot of its own. Made when a material
// first names the pair; a shader that will not load or link is `failed`, and its slot draws
// with the engine's pipeline of its blend.
struct GamePipeCore {
    Name      shader;
    BlendMode blend = BlendMode::Alpha;
    bool      used = false, failed = false;
};

// The full-frame pass as the game set it (renderer_post_layer, renderer_post_params).
struct PostCore {
    Name shader;              // the game's post shader; null: the pass is off
    i32  layer = 0;           // the first layer drawn over it
    bool failed = false;      // the shader would not load or link: off, logged
    f32  params[KV_POST_PARAMS * 4] = {};
    u32  param_count = 0;
};

// A span of host-visible memory for the frame being recorded. It carries its buffer: the
// ring may grow, and retire the buffer the span is in, before the frame is submitted.
struct RingSpan {
    RbBuffer buffer{};
    u64      offset = 0;
    void*    ptr    = nullptr;
};

// The frame ring: one host-visible buffer; a frame's allocations are released when that
// frame is done. Growth retires the old buffer through the garbage queue.
struct FrameRing {
    RbBuffer buf{};
    u64      size  = 0;
    u64      head  = 0; // next free byte
    u64      tail  = 0; // first byte still owned by a frame in flight
    u64      used  = 0; // bytes between tail and head, gaps included
    u64      frame_bytes = 0; // taken by the frame being recorded so far
    u64      peak  = 0;
    u32      grows = 0;
};

struct FrameCore {
    // Destroyed once this slot's next frame is done: what was queued up to its submit
    // (garbage_owned) goes at the slot's next wait; what was queued after it (between
    // frames: a staging buffer, a replaced texture) waits for the frame after, which may use
    // it, as may the other frame still in flight.
    Array<RbGarbage> garbage;
    u32              garbage_owned = 0;
    Array<u32>       dirty_slots;     // texture slots to rewrite in this frame's set
    u64              ring_end   = 0;  // the ring head when this frame was submitted
    u64              ring_bytes = 0;  // what the frame took, released when it is done
    bool             submitted  = false;
};

// The GPU tables and the sort scratch, all sized for `capacity` sprite rows.
struct SpriteTables {
    u32      capacity = 0; // rows; a multiple of kBlockRows
    RbBuffer sprites{};    // GpuSprite[capacity]
    RbBuffer bucket_of{};  // u32[capacity]: each row's bucket, or ~0 when culled or dead
    RbBuffer block_hist{}; // u32[blocks * kBucketCount]: counts, then offsets, per block
    RbBuffer sorted{};     // u32[capacity]: row indices in draw order
    u32      blocks() const { return capacity / kBlockRows; }
};

struct RenderCore {
    Renderer**  owner_slot = nullptr; // the module state's pointer to this; nulled on destroy
    Allocator*  general    = nullptr; // the owning module's attributed general allocator
    char        base_path[512] = {};  // the executable's directory, where shaders/ is
    bool        validation = false;
    bool        vsync      = true;
    u32         want_width = 0, want_height = 0;
    f64         start_time = 0.0;     // the render time's zero

    RenderWorld      world; // the sprites, the materials, what is dirty, the bucket counts (korvid/render/world.h)
    Handle           default_material;
    u16              texture_kind = 0, material_kind = 0;
    Map<u64, Handle> texture_by_name;  // Name::hash -> the first live texture with the name
    u32              textures_made = 0;
    u64              rows_uploaded = 0;

    SpriteTables tables;
    RbBuffer     material_table{}; // GpuMaterial[kMaxMaterials]
    RbBuffer     sort_meta{};      // u32 totals[kBucketCount], starts[kBucketCount], visible
    RbBuffer     indirect{};       // a draw's four words per bucket (VkDrawIndirectCommand's layout)
    FrameRing    ring;

    u32 frame_index = 0;
    u64 frame_count = 0;

    // A requested screenshot: the next presented frame is copied to a host buffer after its
    // draws (the backend's part, which fills in what it copied) and written as a PPM once the
    // frame is done. A dev facility; it stalls.
    char     shot_path[512] = {};
    bool     shot_pending   = false;
    bool     shot_recorded  = false;
    RbBuffer shot_buffer{};
    u32      shot_width = 0, shot_height = 0;
    u32      shot_pitch = 0;       // bytes a row in the buffer; 0: width * 4
    bool     shot_rgba  = false;   // the buffer's bytes are R, G, B, A; else B, G, R, A
    // A requested sort check: the sort's outputs are read back after the frame and verified
    // against the rows' buckets. Dev only; stalls like the screenshot.
    bool     check_pending  = false;
    bool     check_recorded = false;
    RbBuffer check_buffer{};
    u32      check_rows = 0;
    f32*     check_keys = nullptr;  // each row's key as the checked frame computed it
    u32      checks_passed = 0, checks_failed = 0;

    f64 last_wait = 0.0; // seconds blocked in the last renderer_render
};

// ---- the core's functions (core.inl) ---------------------------------------------------------

// The texture and material pools and their handle kinds, the name maps, the world.
bool rc_create_pools(Renderer* r);
void rc_free_pools(Renderer* r);

// A material row as the GPU holds it: the record's, or the default's when the row is dead,
// its texture resolved to a bindless slot. For the frame's upload of dirty rows.
void rc_material_row(Renderer* r, u32 row, GpuMaterial& out);
// The pipeline slot for a material: its blend's, or a game shader's (assigned here, loaded
// and built by rb_game_pipe_load).
u32  rc_material_slot(Renderer* r, Name shader, BlendMode blend);
// The slot is rewritten in each frame's set once that frame is done.
void rc_mark_slot_dirty(Renderer* r, u32 slot);

// Deferred destruction: queued on the slot of the next frame to be submitted (the one being
// recorded, or between frames the one about to be), destroyed after that frame is done, so
// nothing the GPU may still read is freed early. Frames on one queue finish in submission
// order, so that also covers everything earlier frames referenced. What is queued between
// frames may be what the next frame uses (a staging buffer its upload reads) or what the
// frame still in flight reads (a texture replaced meanwhile), so it waits for the next
// frame, not the wait that opens it (slice 7 found the staging buffer freed before its copy).
void rc_defer(Renderer* r, RbGarbage g);
void rc_run_garbage(Renderer* r, FrameInFlight& f); // what the slot's last frame owned; that frame is done

// The frame ring. Allocation is for the frame being recorded; release happens when that
// frame is done. A span is valid until the frame is submitted; its buffer is kept alive
// through the garbage queue if the ring grows meanwhile.
bool     rc_ring_init(Renderer* r, u64 bytes);
RingSpan rc_ring_alloc(Renderer* r, u64 bytes, u64 align);
void     rc_ring_submitted(Renderer* r, FrameInFlight& f); // close the frame's span
void     rc_ring_release(Renderer* r, FrameInFlight& f);   // the frame is done

// --check-sort and --screenshot: the CPU's halves. The backend records the readback (and
// sets check_recorded or shot_recorded); once the frame is done the core verifies or writes.
void rc_snapshot_keys(Renderer* r, f32 alpha); // when the check is recorded
bool rc_verify_sort(Renderer* r);
bool rc_write_screenshot(Renderer* r);

// ---- the backend's functions the core calls --------------------------------------------------

// A buffer's mapping (null when it has none or there is no buffer), and its retirement
// through the garbage queue, which leaves `b` empty.
void* rb_buffer_ptr(const RbBuffer& b);
void  rb_buffer_retire(Renderer* r, RbBuffer& b);
// The ring's buffer: host-visible, mapped, a copy's source, `bytes` long.
bool  rb_ring_buffer(Renderer* r, u64 bytes, RbBuffer& out);
void  rb_garbage_destroy(Renderer* r, const RbGarbage& g);

// The image for t's size and format; any image the slot had retires with the frame.
bool rb_texture_create(Renderer* r, Texture& t);
// Stages the pixels (the ring, or a one-off buffer) and queues the upload for the next frame.
bool rb_stage_texture(Renderer* r, u32 slot, Texture& t, const u8* pixels);
void rb_defer_destroy_texture(Renderer* r, Texture& t);

// Loads the game shader gp.shader (<exe>/shaders/<name> KV_RB_SHADER_EXT) and builds its
// pipeline if the backend can yet (else when it builds the engine's). False when the shader
// did not load; a shader that loads and does not link sets gp.failed.
bool rb_game_pipe_load(Renderer* r, GamePipe& gp);

// The post pass's shader changed (r->post.shader is still the old one): the backend drops
// the old pipeline, loads the new, sets r->post. False: it would not load; the pass is off.
bool rb_post_set(Renderer* r, i32 layer, Name shader);

} // namespace kv
