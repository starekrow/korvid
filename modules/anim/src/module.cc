#include "anim_internal.h"
#include "korvid/asset/scope.h"
#include "korvid/core/format.h"
#include "korvid/core/hash.h"
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
#include <string.h>

namespace kv {

AnimState_* anim_state(Engine& e) { return engine_module_state_as<AnimState_>(e, "anim"); }

namespace {

AnimState_* state(Engine& e) { return anim_state(e); }

void* clip_resolve(kv_handle h, void* ctx) { return ((AnimState_*)ctx)->clips.get(Handle{h}); }
const char* clip_name_of(kv_handle h, void* ctx) { AnimClip* c = ((AnimState_*)ctx)->clips.get(Handle{h}); return c ? c->path : nullptr; }
kv_handle clip_find_name(const char* name, void* ctx) {
    const Handle* h = ((AnimState_*)ctx)->by_path.find(name_lookup(name));
    return h ? h->bits : 0;
}

// What a clip with no name is called: its content, hashed, so the same clip made in any
// order has the same name.
void derive_name(AnimClip& c) {
    u64 h = hash_mix(0x636c6970ull ^ (u64)c.loop ^ ((u64)c.tracks.len << 8));
    h = hash_mix(h ^ hash_bytes(&c.duration, sizeof c.duration));
    for (const AnimTrack& t : c.tracks) h = hash_mix(h ^ (u64)t.channel ^ ((u64)t.interp << 8) ^ ((u64)t.count << 16));
    for (const AnimKey& k : c.keys) h = hash_mix(h ^ hash_bytes(&k.time, sizeof k.time) ^ hash_bytes(k.v, sizeof k.v) * 31 ^ k.event.hash);
    format(c.path, sizeof c.path, "clip:%016llx", (unsigned long long)h);
}

// The asset module's view of a clip: its count, state, size and name; retirement frees it.
u32* clip_refs(void* user, Handle h) { AnimClip* c = ((AnimState_*)user)->clips.get(h); return c ? &c->refs : nullptr; }
AssetState clip_state(void* user, Handle h) { AnimClip* c = ((AnimState_*)user)->clips.get(h); return c ? c->state : AssetState::Failed; }
u64 clip_bytes(void* user, Handle h) {
    AnimClip* c = ((AnimState_*)user)->clips.get(h);
    return c ? (u64)c->keys.cap * sizeof(AnimKey) + (u64)c->tracks.cap * sizeof(AnimTrack) : 0;
}
const char* clip_path(void* user, Handle h) { AnimClip* c = ((AnimState_*)user)->clips.get(h); return c ? c->path : ""; }
void clip_retire(Engine& e, void* user, Handle h) {
    AnimState_& s = *(AnimState_*)user;
    AnimClip* c = s.clips.get(h);
    if (!c) return;
    IoQueue* io = platform_io(e);
    if (io && !c->io.is_null()) io->cancel(c->io);
    Name n = name_lookup(c->path);
    if (const Handle* at = s.by_path.find(n); at && *at == h) s.by_path.remove(n);
    c->tracks.free();
    c->keys.free();
    s.clips.destroy(h);
}
int clip_describe(kv_handle h, void* ctx, char* out, size_t cap) {
    AnimClip* c = ((AnimState_*)ctx)->clips.get(Handle{h});
    if (!c) return format(out, cap, "stale");
    const char* st = c->state == AssetState::Ready ? "ready" : c->state == AssetState::Failed ? "failed" : "pending";
    return format(out, cap, "'%s' %s %.3fs %s, %u tracks, %u players", c->path, st, c->duration, anim_loop_name(c->loop), c->tracks.len, c->players);
}

void fail_clip(Engine& e, AnimState_& s, Handle h, AnimClip& c, AssetError err, const char* detail) {
    c.state = AssetState::Failed;
    c.error = err;
    c.io    = Handle{};
    ++s.failed;
    KV_WARN("anim", "clip '%s': %s%s%s", c.path, asset_error_name(err), detail ? ": " : "", detail ? detail : "");
    e.events.emit(AssetFailed{h, err});
}

// Phase PreSim, main thread: clip files that arrived are parsed, sheets whose texture
// arrived are built.
void anim_pump(TickContext& ctx) {
    AnimState_& s = ctx.state_as<AnimState_>();
    Engine& e = *ctx.engine;
    anim_sheets_pump(e, s);
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
            format(detail, sizeof detail, "%s (byte %u)", why, at);
            fail_clip(e, s, h, *clip, AssetError::Parse, detail);
            continue;
        }
        char path[KV_ASSET_PATH_MAX];
        memcpy(path, clip->path, sizeof path);
        u32 refs = clip->refs;
        *clip = parsed;
        memcpy(clip->path, path, sizeof path);
        clip->refs = refs;
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

    for (KTableRows k : ctx.view<AnimPlayer>()) {
        KTable& table = *k.ktable;
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

bool anim_startup(Engine& e, ModuleInstance& self) {
    AnimState_* s = arena_push<AnimState_>(self.arena);
    if (!s) return false;
    s->general = &self.general;
    s->by_path.init(&self.general);
    kv_handle_kind_desc desc{};
    desc.name = "anim.clip"; desc.type_key = type_key<AnimClip>(); desc.ctx = s;
    desc.resolve = clip_resolve; desc.describe = clip_describe; desc.flags = KV_HANDLE_GENERATIONAL;
    desc.name_of = clip_name_of; desc.find_name = clip_find_name;
    s->kind = handle_kind_register(desc);
    if (!s->kind) return false;
    s->clips.use_kind(s->kind);
    if (!s->clips.init(4096, self.general.tag)) return false;
    AssetKindDesc k;
    k.kind = s->kind; k.refs = clip_refs; k.state = clip_state; k.bytes = clip_bytes; k.path = clip_path; k.retire = clip_retire; k.user = s;
    if (!asset_kind_register(e, k)) return false;
    if (!anim_sheets_init(e, *s, &self.general, self.general.tag)) return false;
    self.set_state(s);
    return true;
}

void anim_shutdown(Engine& e, ModuleInstance& self) {
    AnimState_* s = self.state_as<AnimState_>();
    if (!s) return;
    anim_sheets_shutdown(e, *s);
    asset_kind_unregister(e, s->kind);
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

// /clips: every clip by path (or descriptor name), with its tracks.
const char* clip_state_name(AssetState st) { return st == AssetState::Ready ? "ready" : st == AssetState::Failed ? "failed" : "pending"; }

void clips_list(CliContext& ctx, const char* dir, CliEmitFn emit, void* user) {
    AnimState_* s = state(*ctx.engine);
    CliTree tree{dir, emit, user};
    for (u32 i = 0; s && i < s->clips.len; ++i) {
        if (!s->clips.is_alive(i)) continue;
        const AnimClip& c = s->clips.items[i];
        char summary[96];
        format(summary, sizeof summary, "%s, %.3f s %s, %u tracks, %u players", clip_state_name(c.state), c.duration, anim_loop_name(c.loop), c.tracks.len, c.players);
        if (!tree.add(c.path, false, summary)) return;
    }
}

bool clips_describe(CliContext& ctx, const char* rel) {
    AnimState_* s = state(*ctx.engine);
    for (u32 i = 0; s && i < s->clips.len; ++i) {
        if (!s->clips.is_alive(i) || strcmp(s->clips.items[i].path, rel) != 0) continue;
        const AnimClip& c = s->clips.items[i];
        ctx.print("%s: %s, %.3f s, %s, %u tracks, %u keys, %u players", c.path, clip_state_name(c.state), c.duration, anim_loop_name(c.loop), c.tracks.len, c.keys.len, c.players);
        for (const AnimTrack& t : c.tracks) {
            char line[512];
            usize at = (usize)format(line, sizeof line, "  %-8s %-6s", anim_channel_name(t.channel), t.interp == AnimInterp::Linear ? "linear" : "step");
            for (u32 k = 0; k < t.count && at < sizeof line - 40; ++k) {
                const AnimKey& key = c.keys[t.first + k];
                if (t.channel == AnimChannel::Event) at += (usize)format(line + at, sizeof line - at, " [%g %s]", key.time, name_str(key.event));
                else at += (usize)format(line + at, sizeof line - at, " [%g: %g %g %g %g]", key.time, key.v[0], key.v[1], key.v[2], key.v[3]);
            }
            ctx.print("%s%s", line, t.count && at >= sizeof line - 40 ? " ..." : "");
        }
        return true;
    }
    return false;
}

constinit const CliMountDesc k_mounts[] = {{.path = "/clips", .list = clips_list, .describe = clips_describe}};

constinit const FieldDesc k_player_fields[]   = {KV_FIELDS(AnimPlayer, clip, time, rate, clock, state, fresh, pad, loops)};
constinit const ComponentDesc k_components[]  = {KV_COMPONENT(AnimPlayer, k_player_fields)};
constinit const FieldDesc k_event_fields[]    = {KV_FIELDS(AnimEvent, entity, clip, name)};
constinit const FieldDesc k_finished_fields[] = {KV_FIELDS(AnimFinished, entity, clip)};
constinit const EventTypeDesc k_events[] = {
    KV_EVENT_EX(AnimEvent, k_event_fields, 0),
    KV_EVENT_EX(AnimFinished, k_finished_fields, 0),
};
constinit const ResourceDesc k_resources[]  = {{".clips"}};
constinit const SystemDesc k_systems[] = {
    {.name = ".pump", .phase = Phase::PreSim, .fn = anim_pump, .writes = ".clips", .main_thread = true, .runs = RunWhen::Always},
    {.name = ".play", .phase = Phase::PostSim, .fn = anim_play_system, .reads = ".clips", .writes = "*.AnimPlayer, *.Sprite, *.Local2, *.Transform2",
     .emits = "AnimEvent, AnimFinished", .before = "?scene.compose", .runs = RunWhen::Always},
};

} // namespace

KV_MODULE_DEFINE(anim_module) = {
    .name       = "anim",
    .events     = k_events,
    .components = k_components,
    .resources  = k_resources,
    .systems    = k_systems,
    .mounts     = k_mounts,
    .startup    = anim_startup,
    .shutdown   = anim_shutdown,
    .memory     = {.reserve = 64 * 1024},
};

Handle anim_load(Engine& e, const char* path) { return anim_load(e, path, asset_scope_global(e)); }
Handle anim_create(Engine& e, const AnimClipDesc& desc) { return anim_create(e, desc, asset_scope_global(e)); }

Handle anim_find(Engine& e, const char* name) {
    AnimState_* s = state(e);
    const Handle* h = s && name ? s->by_path.find(name_lookup(name)) : nullptr;
    return h && s->clips.valid(*h) ? *h : Handle{};
}

namespace {

// The scope's reference, as every load takes one; a null scope leaves it to the caller.
Handle held(Engine& e, Handle scope, Handle h) {
    if (!h.is_null() && !scope.is_null() && !asset_scope_add(e, scope, h)) KV_WARN("anim", "clip '%s': the scope is stale", anim_clip(e, h)->path);
    return h;
}

} // namespace

Handle anim_load(Engine& e, const char* path, Handle scope) {
    AnimState_* s = state(e);
    IoQueue* io = platform_io(e);
    if (!s || !io || !path) { KV_ERROR("anim", "cannot load '%s': anim and platform must be up", path ? path : "?"); return Handle{}; }
    if (strlen(path) >= KV_ASSET_PATH_MAX) { KV_ERROR("anim", "path too long: '%s'", path); return Handle{}; }
    Name name = name_intern(path);
    if (const Handle* existing = s->by_path.find(name)) return held(e, scope, *existing);

    AnimClip c{};
    strncpy(c.path, path, sizeof c.path - 1);
    c.state = AssetState::Pending;
    Handle h = s->clips.create(c);
    if (h.is_null()) return Handle{};
    s->by_path.set(name, h);

    IoRequest req;
    req.path  = path;
    req.alloc = s->general;
    req.user  = h.bits;
    Handle iq = io->read_asset(req);
    AnimClip* rec = s->clips.get(h);
    if (iq.is_null()) { fail_clip(e, *s, h, *rec, AssetError::Read, "could not submit"); return held(e, scope, h); }
    rec->io = iq;
    return held(e, scope, h);
}

Handle anim_create(Engine& e, const AnimClipDesc& desc, Handle scope) {
    AnimState_* s = state(e);
    if (!s) { KV_ERROR("anim", "cannot create '%s': anim is not up", desc.name); return Handle{}; }
    AnimClip c;
    const char* why = nullptr;
    if (!anim_build(desc, s->general, c, &why)) { KV_ERROR("anim", "clip '%s': %s", desc.name, why); c.tracks.free(); c.keys.free(); return Handle{}; }
    if (!c.path[0]) derive_name(c);
    Handle h = s->clips.create(c);
    if (h.is_null()) { c.tracks.free(); c.keys.free(); return Handle{}; }
    s->by_path.get_or_insert(name_intern(c.path), h); // the first live one keeps the name
    ++s->created;
    return held(e, scope, h);
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
