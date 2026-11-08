#include "korvid/anim/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/map.h"
#include "korvid/core/memory.h"
#include "korvid/core/pool.h"
#include "korvid/core/profile.h"
#include "korvid/math/fields.h"
#include "korvid/platform/io.h"
#include "korvid/scene/parent.h"
#include "korvid/sprite/sprite.h"
#include "korvid/sprite/transform2.h"
#include <stdio.h>
#include <string.h>

namespace kv {

u32 anim_channel_lanes(AnimChannel c); // clip.cc

namespace {

struct AnimState_ {
    Pool<AnimClip>    clips;
    Map<Name, Handle> by_path;
    Allocator*        general = nullptr;
    u16               kind = 0;
    u32               loaded = 0, failed = 0, created = 0;
    AnimStats         stats;
};

AnimState_* state(Engine& e) { return engine_module_state_as<AnimState_>(e, "anim"); }

void* clip_resolve(kv_handle h, void* ctx) { return ((AnimState_*)ctx)->clips.get(Handle{h}); }
int clip_describe(kv_handle h, void* ctx, char* out, size_t cap) {
    AnimClip* c = ((AnimState_*)ctx)->clips.get(Handle{h});
    if (!c) return snprintf(out, cap, "stale");
    const char* st = c->state == AssetState::Ready ? "ready" : c->state == AssetState::Failed ? "failed" : "pending";
    return snprintf(out, cap, "'%s' %s %.3fs %s, %u tracks, %u players", c->path, st, c->duration, anim_loop_name(c->loop), c->tracks.len, c->players);
}

void fail_clip(Engine& e, AnimState_& s, Handle h, AnimClip& c, AssetError err, const char* detail) {
    c.state = AssetState::Failed;
    c.error = err;
    c.io    = Handle{};
    ++s.failed;
    KV_WARN("anim", "clip '%s': %s%s%s", c.path, asset_error_name(err), detail ? ": " : "", detail ? detail : "");
    e.events.emit(AssetFailed{h, err});
}

// Phase PreSim, main thread: clip files that arrived are parsed.
void anim_pump(TickContext& ctx) {
    AnimState_& s = ctx.state_as<AnimState_>();
    Engine& e = *ctx.engine;
    IoQueue* io = platform_io(e);
    if (!io) return;
    for (u32 i = 0; i < io->completions().len;) {
        IoCompletion c = io->completions()[i];
        if (Handle{c.user}.kind() != s.kind || !s.kind) { ++i; continue; }
        Handle h{c.user};
        AnimClip* clip = s.clips.get(h);
        if (!clip || clip->io != c.request) { ++i; continue; }
        io->reap(c.request);
        clip->io = Handle{};
        if (c.status != IoStatus::Done) {
            fail_clip(e, s, h, *clip, c.status == IoStatus::Failed && c.error == 2 ? AssetError::NotFound : AssetError::Read, nullptr);
            continue;
        }
        AnimClip parsed;
        const char* why = nullptr;
        u32 at = 0;
        bool ok = anim_parse((const char*)c.data, c.size, s.general, parsed, &why, &at);
        free_in(s.general, c.data, c.size);
        if (!ok) {
            char detail[160];
            snprintf(detail, sizeof detail, "%s (byte %u)", why, at);
            fail_clip(e, s, h, *clip, AssetError::Parse, detail);
            continue;
        }
        char path[KV_ASSET_PATH_MAX];
        memcpy(path, clip->path, sizeof path);
        *clip = parsed;
        memcpy(clip->path, path, sizeof path);
        ++s.loaded;
        KV_INFO("anim", "clip '%s' ready: %.3fs %s, %u tracks, %u keys", clip->path, clip->duration, anim_loop_name(clip->loop), clip->tracks.len, clip->keys.len);
        e.events.emit(AssetReady{h});
    }
}

// Event keys with times in the interval a player just moved through: (t0, t1] forward,
// [t1, t0) backward, closed at the start when `closed` (the first advance of a play or
// a loop).
void fire_events(TickContext& ctx, Handle entity, Handle clip_h, const AnimClip& c, f32 a, f32 b, bool closed, bool forward, AnimStats& s) {
    for (const AnimTrack& t : c.tracks) {
        if (t.channel != AnimChannel::Event) continue;
        for (u32 i = 0; i < t.count; ++i) {
            const AnimKey& k = c.keys[t.first + i];
            bool in = forward ? ((closed ? k.time >= a : k.time > a) && k.time <= b)
                              : (k.time >= a && (closed ? k.time <= b : k.time < b));
            if (!in) continue;
            ctx.emit(AnimEvent{entity, clip_h, k.event.hash});
            ++s.events;
        }
    }
}

// Moves a player by dt * rate through the clip, segment by segment across the ends.
void advance(TickContext& ctx, Handle entity, AnimPlayer& p, const AnimClip& c, f32 dt, AnimStats& s) {
    f32 D = c.duration;
    bool closed = p.fresh != 0;
    p.fresh = 0;
    if (D <= 0.0f) {
        if (closed) fire_events(ctx, entity, p.clip, c, 0.0f, 0.0f, true, true, s);
        if (c.loop == AnimLoop::Once) { p.state = AnimState::Done; ctx.emit(AnimFinished{entity, p.clip}); ++s.finished; }
        p.time = 0.0f;
        return;
    }
    f32 remaining = dt * p.rate;
    for (u32 guard = 0; remaining != 0.0f && guard < 64; ++guard) {
        f32 t0 = p.time;
        if (remaining > 0.0f) {
            f32 t1 = t0 + remaining;
            bool hit = t1 >= D;
            if (hit) t1 = D;
            fire_events(ctx, entity, p.clip, c, t0, t1, closed, true, s);
            remaining -= t1 - t0;
            p.time = t1;
            closed  = false;
            if (!hit) break;
            if (c.loop == AnimLoop::Once) { p.state = AnimState::Done; ctx.emit(AnimFinished{entity, p.clip}); ++s.finished; break; }
            if (c.loop == AnimLoop::Loop) { p.time = 0.0f; ++p.loops; closed = true; }
            else { p.rate = -p.rate; remaining = -remaining; }
        } else {
            f32 t1 = t0 + remaining;
            bool hit = t1 <= 0.0f;
            if (hit) t1 = 0.0f;
            fire_events(ctx, entity, p.clip, c, t1, t0, closed, false, s);
            remaining -= t1 - t0;
            p.time = t1;
            closed  = false;
            if (!hit) break;
            if (c.loop == AnimLoop::Once) { p.state = AnimState::Done; ctx.emit(AnimFinished{entity, p.clip}); ++s.finished; break; }
            if (c.loop == AnimLoop::Loop) { p.time = D; ++p.loops; closed = true; }
            else { p.rate = -p.rate; remaining = -remaining; ++p.loops; }
        }
    }
    // Loop and PingPong keep the time inside [0, D); a Loop that wrapped to exactly D in
    // reverse shows the end, which is the same frame as the start of the next pass.
}

// Phase PostSim, every tick: advance, then write the channels.
void anim_play_system(TickContext& ctx) {
    KV_ZONE("anim.play");
    AnimState_& st = ctx.state_as<AnimState_>();
    AnimStats s;
    f32 dt = (f32)ctx.dt;
    for (u32 i = 0; i < st.clips.len; ++i) if (st.clips.is_alive(i)) st.clips.items[i].players = 0;

    for (KindRows k : ctx.view<AnimPlayer>()) {
        Table& table = *k.table;
        Span<AnimPlayer> players = k.col<AnimPlayer>();
        bool has_sprite = table.has(ComponentType<Sprite>::id);
        bool has_local  = table.has(ComponentType<Local2>::id) && table.has(ComponentType<Parent>::id);
        bool has_xf     = table.has(ComponentType<Transform2>::id);
        Span<Sprite>     sprites = has_sprite ? k.col<Sprite>() : Span<Sprite>();
        Span<Local2>     locals  = has_local ? k.col<Local2>() : Span<Local2>();
        Span<Transform2> xfs     = has_xf ? k.col<Transform2>() : Span<Transform2>();
        for (u32 i = 0; i < k.rows(); ++i) {
            if (!k.alive(i)) continue;
            AnimPlayer& p = players[i];
            ++s.players;
            AnimClip* c = st.clips.get(p.clip);
            if (!c || c->state != AssetState::Ready) continue;
            ++c->players;
            if (p.state != AnimState::Playing) continue;
            if (p.clock == AnimClock::Sim && ctx.paused) continue;
            ++s.advanced;
            advance(ctx, k.handle_at(i), p, *c, dt, s);

            for (const AnimTrack& t : c->tracks) {
                f32 v[4];
                if (!anim_sample(*c, t, p.time, v)) continue;
                switch (t.channel) {
                case AnimChannel::Uv:       if (has_sprite) sprites[i].uv = rect2{v[0], v[1], v[2], v[3]}; break;
                case AnimChannel::Tint:     if (has_sprite) sprites[i].tint = color_rgba(v[0], v[1], v[2], v[3]); break;
                case AnimChannel::Alpha:    if (has_sprite) sprites[i].tint.a = v[0]; break;
                case AnimChannel::Position:
                    if (has_local) locals[i].position = point2{v[0], v[1]};
                    else if (has_xf) xfs[i].position = point2{v[0], v[1]};
                    break;
                case AnimChannel::Rotation:
                    if (has_local) locals[i].rotation = v[0];
                    else if (has_xf) xfs[i].rotation = v[0];
                    break;
                case AnimChannel::Scale:
                    if (has_local) locals[i].scale = vec2{v[0], v[1]};
                    else if (has_xf) xfs[i].scale = vec2{v[0], v[1]};
                    break;
                default: break;
                }
            }
        }
    }
    st.stats = s;
}

bool anim_startup(Engine&, ModuleInstance& self) {
    AnimState_* s = arena_push<AnimState_>(self.arena);
    if (!s) return false;
    s->general = &self.general;
    s->by_path.init(&self.general);
    kv_handle_kind_desc desc{};
    desc.name = "anim.clip"; desc.type_key = type_key<AnimClip>(); desc.ctx = s;
    desc.resolve = clip_resolve; desc.describe = clip_describe; desc.flags = KV_HANDLE_GENERATIONAL;
    s->kind = handle_kind_register(desc);
    if (!s->kind) return false;
    s->clips.use_kind(s->kind);
    if (!s->clips.init(4096, self.general.tag)) return false;
    self.set_state(s);
    return true;
}

void anim_shutdown(Engine& e, ModuleInstance& self) {
    AnimState_* s = self.state_as<AnimState_>();
    if (!s) return;
    IoQueue* io = platform_io(e);
    for (u32 i = 0; i < s->clips.len; ++i) {
        if (!s->clips.is_alive(i)) continue;
        AnimClip& c = s->clips.items[i];
        if (io && !c.io.is_null()) io->cancel(c.io);
        c.tracks.free();
        c.keys.free();
    }
    KV_INFO("anim", "%u clips loaded, %u failed, %u created", s->loaded, s->failed, s->created);
    kv_handle_kind_unregister(s->kind);
    s->by_path.free();
    s->clips.free();
}

constinit const FieldDesc k_player_fields[]   = {KV_FIELDS(AnimPlayer, clip, time, rate, clock, state, fresh, pad, loops)};
constinit const ComponentDesc k_components[]  = {KV_COMPONENT(AnimPlayer, k_player_fields)};
constinit const FieldDesc k_event_fields[]    = {KV_FIELDS(AnimEvent, entity, clip, name)};
constinit const FieldDesc k_finished_fields[] = {KV_FIELDS(AnimFinished, entity, clip)};
constinit const EventTypeDesc k_events[] = {
    KV_EVENT_EX(AnimEvent, k_event_fields, 0),
    KV_EVENT_EX(AnimFinished, k_finished_fields, 0),
};
constinit const ResourceDesc k_resources[]  = {{"anim.clips"}};
constinit const char* const  k_pump_writes[] = {"anim.clips"};
constinit const char* const  k_play_reads[]  = {"anim.clips"};
constinit const char* const  k_play_writes[] = {"*.AnimPlayer", "*.Sprite", "*.Local2", "*.Transform2"};
constinit const char* const  k_play_after[]  = {"?text.layout"};
constinit const char* const  k_play_before[] = {"?scene.compose"};
constinit const SystemDesc k_systems[] = {
    {.name = "anim.pump", .phase = Phase::PreSim, .fn = anim_pump, .writes = k_pump_writes, .main_thread = true, .runs = RunWhen::Always},
    {.name = "anim.play", .phase = Phase::PostSim, .fn = anim_play_system, .reads = k_play_reads, .writes = k_play_writes,
     .after = k_play_after, .before = k_play_before, .runs = RunWhen::Always},
};

} // namespace

KV_MODULE_DEFINE(anim_module) = {
    .name       = "anim",
    .events     = k_events,
    .components = k_components,
    .resources  = k_resources,
    .systems    = k_systems,
    .startup    = anim_startup,
    .shutdown   = anim_shutdown,
    .memory     = {.reserve = 64 * 1024},
};

Handle anim_load(Engine& e, const char* path) {
    AnimState_* s = state(e);
    IoQueue* io = platform_io(e);
    if (!s || !io || !path) { KV_ERROR("anim", "cannot load '%s': anim and platform must be up", path ? path : "?"); return Handle{}; }
    if (strlen(path) >= KV_ASSET_PATH_MAX) { KV_ERROR("anim", "path too long: '%s'", path); return Handle{}; }
    Name name = name_intern(path);
    if (const Handle* existing = s->by_path.find(name)) return *existing;

    AnimClip c{};
    strncpy(c.path, path, sizeof c.path - 1);
    c.state = AssetState::Pending;
    Handle h = s->clips.create(c);
    if (h.is_null()) return Handle{};
    s->by_path.set(name, h);

    char full[512];
    snprintf(full, sizeof full, "%sassets/%s", platform_base_path(e), path);
    IoRequest req;
    req.path  = full;
    req.alloc = s->general;
    req.user  = h.bits;
    Handle iq = io->read_file(req);
    AnimClip* rec = s->clips.get(h);
    if (iq.is_null()) { fail_clip(e, *s, h, *rec, AssetError::Read, "could not submit"); return h; }
    rec->io = iq;
    return h;
}

Handle anim_create(Engine& e, const AnimClipDesc& desc) {
    AnimState_* s = state(e);
    if (!s) { KV_ERROR("anim", "cannot create '%s': anim is not up", desc.name); return Handle{}; }
    AnimClip c;
    const char* why = nullptr;
    if (!anim_build(desc, s->general, c, &why)) { KV_ERROR("anim", "clip '%s': %s", desc.name, why); c.tracks.free(); c.keys.free(); return Handle{}; }
    Handle h = s->clips.create(c);
    if (h.is_null()) { c.tracks.free(); c.keys.free(); return Handle{}; }
    ++s->created;
    return h;
}

const AnimClip* anim_clip(Engine& e, Handle clip) {
    AnimState_* s = state(e);
    return s ? s->clips.get(clip) : nullptr;
}

const AnimStats* anim_stats(Engine& e) {
    AnimState_* s = state(e);
    return s ? &s->stats : nullptr;
}

} // namespace kv
