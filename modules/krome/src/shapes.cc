// Krome's shapes (docs/PRDs/Krome-Slice-5-PRD.md, "Krome"): an image node whose asset is a shape
// is rasterized by patherizer at the node's pixel size and kept in pages, as glyphs are, by
// shape, size and colour. A one-colour shape (every paint currentColor) is rasterized once in
// white and tinted by the box, so a hover, a theme or a transition changes the tint and not the
// pixels; a shape with colours of its own is rasterized with currentColor resolved, and that
// colour is part of its key. A page's changed rectangle goes to the renderer after the paint.
//
// A shape of more than 256 x 256 pixels costs frames to rasterize (an illustration at 1000 px is
// 14 to 48 ms), so it goes to a task worker of the platform's queue and lands in a page when it
// is done: the node draws nothing until then, or, while a new size or colour of a shape already
// drawn is on its way, the old picture stretched. The queue's pending task keeps a loop that
// paces on demand ticking until it lands. Without task workers it is rasterized here.
#include "internal.h"
#include "korvid/asset/shape.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/time.h"
#include "korvid/platform/io.h"
#include "korvid/render/renderer.h"
#include <new>
#include <string.h>

namespace kv {

namespace {

constexpr u32 kPage = 1024;      // a page's side, pixels
constexpr u32 kOwnPast = 512;    // a shape wider or taller than this gets a texture of its own
constexpr u32 kMaxPages = 8;     // past it, the page used longest ago is cleared and reused
constexpr u64 kJobPast = 256 * 256; // a shape of more pixels than this is rasterized on a job
constexpr u32 kJobWorkers = 2;   // the platform queue's task workers, started the first time
constexpr u8  kJobLane = 0;

u32 pack(color c) {
    auto b = [](f32 v) { return (u32)(v <= 0.0f ? 0 : v >= 1.0f ? 255 : (u32)(v * 255.0f + 0.5f)); };
    return b(c.r) | b(c.g) << 8 | b(c.b) << 16 | b(c.a) << 24;
}

// Room for w x h on a page: its place, or false when the page is full.
bool place(KromeShapePage& p, u32 w, u32 h, u32* x, u32* y) {
    if (p.x + w > kPage) { p.x = 0; p.shelf_y += p.shelf_h + 1; p.shelf_h = 0; }
    if (p.shelf_y + h > kPage) return false;
    *x = p.x;
    *y = p.shelf_y;
    p.x += w + 1;
    if (h > p.shelf_h) p.shelf_h = h;
    return true;
}

void dirty(KromeShapePage& p, u32 x, u32 y, u32 w, u32 h) {
    if (!p.dirty) { p.d0x = x; p.d0y = y; p.d1x = x + w; p.d1y = y + h; p.dirty = true; return; }
    p.d0x = x < p.d0x ? x : p.d0x; p.d0y = y < p.d0y ? y : p.d0y;
    p.d1x = x + w > p.d1x ? x + w : p.d1x; p.d1y = y + h > p.d1y ? y + h : p.d1y;
}

bool new_page(KromeModule& s, KromeShapePage& p, u32 w, u32 h) {
    usize bytes = (usize)w * h * 4;
    p.rgba = (u8*)alloc_in(s.general, bytes, 16);
    if (!p.rgba) return false;
    memset(p.rgba, 0, bytes);
    p.w = w;
    p.h = h;
    return true;
}

// A page's texture, made the first time it is drawn with a renderer: the whole page once, so
// the texture has its size; then only what changes.
bool page_texture(KromeModule& s, Renderer* r, KromeShapePage& p) {
    if (!p.texture.is_null()) return true;
    char name[48];
    format(name, sizeof name, "krome:shapes%u", s.shape_pages_made++);
    TextureDesc td;
    td.format = TextureFormat::RGBA8_SRGB; // patherizer writes sRGB, straight alpha
    td.name = name;
    p.texture = renderer_texture_create(r, td);
    if (p.texture.is_null()) return false;
    renderer_texture_update(r, p.texture, p.rgba, p.w, p.h, 0, 0, p.w, p.h);
    p.dirty = false;
    return true;
}

// The box drawing entry e: false without a renderer (the cache is kept all the same).
bool boxed(KromeModule& s, Renderer* r, KromeShapePage& p, const KromeShapeEntry& e, UiBox& box) {
    if (!r || !page_texture(s, r, p)) return false;
    box.textured = true;
    box.texture = renderer_texture_slot(r, p.texture);
    box.uv = rect2{(f32)e.x / (f32)p.w, (f32)e.y / (f32)p.h, (f32)e.w / (f32)p.w, (f32)e.h / (f32)p.h};
    return true;
}

// A page freed, its slot left empty (rgba null) for the next page made: entries name pages by
// their index.
void free_page(KromeModule& s, Renderer* r, KromeShapePage& p) {
    if (r && !p.texture.is_null()) renderer_texture_destroy(r, p.texture);
    if (p.rgba) free_in(s.general, p.rgba, (usize)p.w * p.h * 4);
    p = KromeShapePage{};
}

void drop_entries(KromeModule& s, u32 page) {
    for (u32 i = 0; i < s.shape_entries.len;) {
        if (s.shape_entries[i].page == page) { s.shape_entries[i] = s.shape_entries.last(); --s.shape_entries.len; }
        else ++i;
    }
}

// A place for w x h: a page with room, a new one, or the one used longest ago, cleared. A large
// shape gets a page of its own, and the large shapes' pages used longest ago are freed while
// they hold more than the budget. ~0u when there is no memory.
u32 room(KromeModule& s, Renderer* r, u32 w, u32 h, u32* x, u32* y) {
    u32 page = ~0u;
    bool own = w > kOwnPast || h > kOwnPast;
    if (!own) {
        u32 shared = 0;
        for (u32 i = 0; i < s.shape_pages.len && page == ~0u; ++i) {
            KromeShapePage& p = s.shape_pages[i];
            if (!p.rgba || p.own) continue;
            ++shared;
            if (place(p, w, h, x, y)) page = i;
        }
        if (page == ~0u && shared >= kMaxPages) {
            u32 oldest = ~0u;
            for (u32 i = 0; i < s.shape_pages.len; ++i) {
                const KromeShapePage& p = s.shape_pages[i];
                if (p.rgba && !p.own && (oldest == ~0u || p.used < s.shape_pages[oldest].used)) oldest = i;
            }
            if (oldest != ~0u) {
                KromeShapePage& p = s.shape_pages[oldest];
                drop_entries(s, oldest);
                memset(p.rgba, 0, (usize)p.w * p.h * 4);
                p.x = p.shelf_y = p.shelf_h = 0;
                dirty(p, 0, 0, p.w, p.h);
                if (place(p, w, h, x, y)) page = oldest;
            }
        }
        if (page != ~0u) return page;
    } else {
        usize kept = 0, bytes = (usize)w * h * 4;
        for (const KromeShapePage& p : s.shape_pages) if (p.rgba && p.own) kept += (usize)p.w * p.h * 4;
        while (kept + bytes > s.shape_own_budget) {
            u32 oldest = ~0u;
            for (u32 i = 0; i < s.shape_pages.len; ++i) {
                const KromeShapePage& p = s.shape_pages[i];
                if (p.rgba && p.own && p.used <= s.shape_frame && (oldest == ~0u || p.used < s.shape_pages[oldest].used)) oldest = i;
            }
            if (oldest == ~0u) break; // the rest are drawn this frame: over the budget until the next
            KromeShapePage& p = s.shape_pages[oldest];
            kept -= (usize)p.w * p.h * 4;
            drop_entries(s, oldest);
            free_page(s, r, p);
        }
    }
    KromeShapePage p;
    p.own = own;
    if (!new_page(s, p, own ? w : kPage, own ? h : kPage)) return ~0u;
    *x = *y = 0;
    if (!own && !place(p, w, h, x, y)) { free_page(s, r, p); return ~0u; }
    for (u32 i = 0; i < s.shape_pages.len; ++i)
        if (!s.shape_pages[i].rgba) { s.shape_pages[i] = p; return i; }
    s.shape_pages.push(p);
    return s.shape_pages.len - 1;
}

KromeShapeEntry& keep(KromeModule& s, Handle asset, u32 w, u32 h, u32 colour, u32 page, u32 x, u32 y) {
    KromeShapePage& p = s.shape_pages[page];
    p.used = s.shape_clock;
    dirty(p, x, y, w, h);
    ++s.shapes_drawn;
    KromeShapeEntry e;
    e.asset = asset;
    e.w = w;
    e.h = h;
    e.colour = colour;
    e.page = page;
    e.x = x;
    e.y = y;
    e.used = s.shape_clock;
    s.shape_entries.push(e);
    return s.shape_entries.last();
}

PathOptions options_for(u32 colour) {
    PathOptions o;
    o.current[0] = (u8)colour; o.current[1] = (u8)(colour >> 8); o.current[2] = (u8)(colour >> 16); o.current[3] = (u8)(colour >> 24);
    return o;
}

} // namespace

// ---- jobs ----

struct KromeShapeJob {
    PathShape   shape;    // a copy: the asset may be reloaded or freed while the worker draws
    PathOptions options;
    u8*         px = nullptr; // w x h, the worker's until the completion is reaped
    u32         w = 0, h = 0, colour = 0;
    Handle      asset, task;
    bool        dropped = false; // a newer size or colour asked for: what it draws is not kept
};

namespace {

template <typename T> bool copy_array(Array<T>& to, const Array<T>& from) {
    to.clear();
    if (!from.len) return true;
    T* p = to.try_push_n(from.len);
    if (!p) return false;
    memcpy(p, from.data, (usize)from.len * sizeof(T));
    return true;
}

bool copy_shape(PathShape& to, const PathShape& from, Allocator* a) {
    to.init(a);
    memcpy(to.view, from.view, sizeof to.view);
    return copy_array(to.verbs, from.verbs) && copy_array(to.points, from.points) && copy_array(to.paints, from.paints)
        && copy_array(to.stops, from.stops) && copy_array(to.strokes, from.strokes) && copy_array(to.dashes, from.dashes)
        && copy_array(to.nodes, from.nodes) && copy_array(to.clips, from.clips);
}

void free_job(KromeModule& s, KromeShapeJob* j) {
    j->shape.free();
    if (j->px) free_in(s.general, j->px, (usize)j->w * j->h * 4);
    j->~KromeShapeJob();
    free_in(s.general, j, sizeof *j);
}

// On a task worker: the job touches only itself.
i32 shape_task(void* arg, const IoTaskCtl& ctl) {
    KromeShapeJob& j = *(KromeShapeJob*)arg;
    if (ctl.cancelled()) return 125; // ECANCELED
    return patherizer_draw(j.shape, PathTarget{j.px, j.w, j.h, j.w * 4, false}, j.options) ? 0 : 12; // ENOMEM
}

IoQueue* job_queue(KromeModule& s) {
    IoQueue* io = s.e ? platform_io(*s.e) : nullptr;
    if (!io) return nullptr;
    if (!io->tasks) io->start_tasks(kJobWorkers);
    return io->tasks ? io : nullptr;
}

// The shape handed to a worker: false when there is none (or no memory), and the caller
// rasterizes it itself.
bool start_job(KromeModule& s, const ShapeAsset& shape, Handle asset, u32 w, u32 h, u32 colour) {
    IoQueue* io = job_queue(s);
    if (!io) return false;
    void* mem = alloc_in(s.general, sizeof(KromeShapeJob), alignof(KromeShapeJob));
    if (!mem) return false;
    KromeShapeJob* j = new (mem) KromeShapeJob;
    j->w = w;
    j->h = h;
    j->colour = colour;
    j->asset = asset;
    j->options = options_for(colour);
    j->px = (u8*)alloc_in(s.general, (usize)w * h * 4, 16);
    if (!j->px || !copy_shape(j->shape, shape.shape, s.general)) { free_job(s, j); return false; }
    memset(j->px, 0, (usize)w * h * 4);
    IoTaskRequest req;
    req.fn = shape_task;
    req.arg = j;
    req.name = "krome shape";
    req.lane = kJobLane;
    j->task = io->run_task(req);
    if (j->task.is_null()) { free_job(s, j); return false; }
    // What was on its way at another size in this colour is not wanted any more.
    for (KromeShapeJob* o : s.shape_jobs)
        if (!o->dropped && o->asset == asset && o->colour == colour) { io->cancel(o->task); o->dropped = true; }
    s.shape_jobs.alloc = s.general;
    s.shape_jobs.push(j);
    ++s.shape_jobs_run;
    return true;
}

// While a job runs: the same shape drawn last at this size or in this colour, stretched.
bool stand_in(KromeModule& s, Renderer* r, Handle asset, u32 w, u32 h, u32 colour, UiBox& box) {
    KromeShapeEntry* best = nullptr;
    for (KromeShapeEntry& e : s.shape_entries)
        if (e.asset == asset && (e.colour == colour || (e.w == w && e.h == h)) && (!best || e.used > best->used)) best = &e;
    if (!best) return false;
    ++s.shapes_stood_in;
    best->used = s.shape_clock;
    KromeShapePage& p = s.shape_pages[best->page];
    p.used = s.shape_clock;
    return boxed(s, r, p, *best, box);
}

} // namespace

bool krome_shape_box(KromeModule& s, Renderer* r, const ShapeAsset& shape, Handle asset, u32 w, u32 h, color current, UiBox& box) {
    if (!w || !h || w > 4096 || h > 4096) return false;
    u32 colour = shape.one_colour ? 0xFFFFFFFFu : pack(current);
    s.shape_entries.alloc = s.general;
    s.shape_pages.alloc = s.general;
    ++s.shape_clock;
    for (KromeShapeEntry& e : s.shape_entries) {
        if (e.asset == asset && e.w == w && e.h == h && e.colour == colour) {
            e.used = s.shape_clock;
            KromeShapePage& p = s.shape_pages[e.page];
            p.used = s.shape_clock;
            return boxed(s, r, p, e, box);
        }
    }
    // Large: on a job, if it is not on one already.
    if ((u64)w * h > kJobPast) {
        bool running = false;
        for (KromeShapeJob* j : s.shape_jobs)
            if (!j->dropped && j->asset == asset && j->w == w && j->h == h && j->colour == colour) running = true;
        if (running || start_job(s, shape, asset, w, h, colour)) return stand_in(s, r, asset, w, h, colour, box);
    }
    u32 x = 0, y = 0;
    u32 page = room(s, r, w, h, &x, &y);
    if (page == ~0u) return false;
    KromeShapePage& p = s.shape_pages[page];
    // Drawn into its place on the page: cleared, then the shape over it.
    for (u32 row = 0; row < h; ++row) memset(p.rgba + ((usize)(y + row) * p.w + x) * 4, 0, (usize)w * 4);
    patherizer_draw(shape.shape, PathTarget{p.rgba + ((usize)y * p.w + x) * 4, w, h, p.w * 4, false}, options_for(colour));
    KromeShapeEntry& e = keep(s, asset, w, h, colour, page, x, y);
    return boxed(s, r, p, e, box);
}

bool krome_shapes_land(KromeModule& s, Renderer* r) {
    s.shape_frame = s.shape_clock;
    if (!s.shape_jobs.len) return false;
    IoQueue* io = s.e ? platform_io(*s.e) : nullptr;
    if (!io) return false;
    bool landed = false;
    for (u32 i = 0; i < s.shape_jobs.len;) {
        KromeShapeJob* j = s.shape_jobs[i];
        IoCompletion done;
        if (!io->take(j->task, &done)) { ++i; continue; } // still on its way
        IoStatus st = done.status;
        i32 err = done.error;
        if (st == IoStatus::Done && !j->dropped) {
            ++s.shape_clock;
            u32 x = 0, y = 0;
            u32 page = room(s, r, j->w, j->h, &x, &y);
            if (page != ~0u) {
                KromeShapePage& p = s.shape_pages[page];
                for (u32 row = 0; row < j->h; ++row)
                    memcpy(p.rgba + ((usize)(y + row) * p.w + x) * 4, j->px + (usize)row * j->w * 4, (usize)j->w * 4);
                keep(s, j->asset, j->w, j->h, j->colour, page, x, y);
                landed = true;
            }
        } else if (st == IoStatus::Failed) {
            KV_WARN("krome", "a shape at %u x %u was not rasterized (error %d)", j->w, j->h, err);
        }
        free_job(s, j);
        s.shape_jobs[i] = s.shape_jobs.last();
        --s.shape_jobs.len;
    }
    return landed;
}

void krome_shapes_flush(KromeModule& s, Renderer* r) {
    if (!r) return;
    for (KromeShapePage& p : s.shape_pages) {
        if (!p.dirty || p.texture.is_null()) continue;
        renderer_texture_update(r, p.texture, p.rgba, p.w, p.h, p.d0x, p.d0y, p.d1x - p.d0x, p.d1y - p.d0y);
        p.dirty = false;
    }
}

void krome_shapes_free(KromeModule& s, Renderer* r) {
    // A job's pixels are its worker's until its completion is reaped: cancelled, then waited for.
    IoQueue* io = s.e ? platform_io(*s.e) : nullptr;
    if (io) {
        for (KromeShapeJob* j : s.shape_jobs) { io->cancel(j->task); j->dropped = true; }
        for (f64 until = time_now() + 10.0; s.shape_jobs.len && time_now() < until;) {
            io->pump();
            krome_shapes_land(s, r);
            if (s.shape_jobs.len) time_sleep(0.001);
        }
    }
    if (s.shape_jobs.len) KV_WARN("krome", "%u shape jobs did not finish: their memory is left", s.shape_jobs.len);
    s.shape_jobs.free();
    for (KromeShapePage& p : s.shape_pages) free_page(s, r, p);
    s.shape_pages.free();
    s.shape_entries.free();
}

} // namespace kv
