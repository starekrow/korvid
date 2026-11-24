// Scopes and references: who holds which asset, and what happens when nobody does.
#include "korvid/asset/scope.h"
#include "korvid/core/format.h"
#include "korvid/core/cli.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/utf8.h"
#include "korvid/platform/io.h"
#include "asset_internal.h"
#include <string.h>

namespace kv {

namespace {

const AssetKindDesc* external_kind(AssetState_& s, u16 kind) {
    for (u32 i = 0; i < s.external_count; ++i) if (s.external[i].kind == kind) return &s.external[i];
    return nullptr;
}

AssetState read_state(AssetState_& s, Handle h, bool* alive) {
    *alive = true;
    u16 k = h.kind();
    if (k == s.font_kind)          { if (FontAsset* f = s.fonts.get(h))      return f->state; }
    else if (k == s.sound_kind)    { if (SoundAsset* d = s.sounds.get(h))    return d->state; }
    else if (k == s.blob_kind)     { if (BlobAsset* b = s.blobs.get(h))      return b->state; }
    else if (k == s.shape_kind)    { if (ShapeAsset* b = s.shapes.get(h))    return b->state; }
    else if (k == s.stream_kind)   { if (StreamAsset* t = s.streams.get(h))  return t->state; }
    else if (const AssetKindDesc* x = external_kind(s, k)) { if (x->refs(x->user, h)) return x->state(x->user, h); }
    *alive = false;
    return AssetState::Failed;
}

u64 bytes_of(AssetState_& s, Handle h) {
    u16 k = h.kind();
    if (k == s.font_kind) {
        if (FontAsset* f = s.fonts.get(h)) {
            u32 glyphs = 0, pages = 0;
            font_counts(*f, &glyphs, &pages);
            return (u64)pages * KV_FONT_PAGE * KV_FONT_PAGE * 4 + (u64)glyphs * (sizeof(Glyph) + 16) + (u64)f->kern_count * (sizeof(u64) + sizeof(f32));
        }
    } else if (k == s.sound_kind) { if (SoundAsset* d = s.sounds.get(h)) return (u64)d->frames * d->channels * sizeof(f32); }
    else if (k == s.blob_kind) { if (BlobAsset* b = s.blobs.get(h)) return b->size; }
    else if (k == s.shape_kind) {
        if (ShapeAsset* b = s.shapes.get(h))
            return (u64)b->shape.points.len * 4 + b->shape.verbs.len + (u64)b->shape.nodes.len * sizeof(PathNode) + (u64)b->shape.paints.len * sizeof(PathPaint);
    }
    else if (k == s.stream_kind) { if (StreamAsset* t = s.streams.get(h)) return t->size; }
    else if (const AssetKindDesc* x = external_kind(s, k)) { if (x->bytes && x->refs(x->user, h)) return x->bytes(x->user, h); }
    return 0;
}

const char* path_of(AssetState_& s, Handle h) {
    u16 k = h.kind();
    if (k == s.font_kind)          { if (FontAsset* f = s.fonts.get(h))         return f->path; }
    else if (k == s.sound_kind)    { if (SoundAsset* d = s.sounds.get(h))       return d->path; }
    else if (k == s.blob_kind)     { if (BlobAsset* b = s.blobs.get(h))         return b->path; }
    else if (k == s.shape_kind)    { if (ShapeAsset* b = s.shapes.get(h))       return b->path; }
    else if (k == s.stream_kind)   { if (StreamAsset* t = s.streams.get(h))     return t->path; }
    else if (const AssetKindDesc* x = external_kind(s, k)) { if (x->path && x->refs(x->user, h)) return x->path(x->user, h); }
    return "?";
}

AssetScopeProgress progress_of(AssetState_& s, const AssetScope_& sc) {
    AssetScopeProgress p;
    for (Handle h : sc.members) {
        bool alive;
        AssetState st = read_state(s, h, &alive);
        ++p.members;
        if (!alive || st == AssetState::Failed) ++p.failed;
        else if (st == AssetState::Ready) ++p.ready;
        else ++p.pending;
        p.bytes += bytes_of(s, h);
    }
    return p;
}

void* scope_resolve(kv_handle h, void* ctx) { return ((AssetState_*)ctx)->scopes.get(Handle{h}); }
const char* scope_name_of(kv_handle h, void* ctx) { AssetScope_* sc = ((AssetState_*)ctx)->scopes.get(Handle{h}); return sc ? sc->name : nullptr; }
kv_handle scope_find_name(const char* name, void* ctx) {
    AssetState_& s = *(AssetState_*)ctx;
    for (u32 i = 0; i < s.scopes.len; ++i)
        if (s.scopes.is_alive(i) && strcmp(s.scopes.items[i].name, name) == 0) return s.scopes.handle_at(i).bits;
    return 0;
}
int scope_describe(kv_handle h, void* ctx, char* out, size_t cap) {
    AssetState_& s = *(AssetState_*)ctx;
    AssetScope_* sc = s.scopes.get(Handle{h});
    if (!sc) return format(out, cap, "stale");
    AssetScopeProgress p = progress_of(s, *sc);
    return format(out, cap, "'%s' %u assets, %u pending, %u failed", sc->name, p.members, p.pending, p.failed);
}

void retire(Engine& e, AssetState_& s, Handle h) {
    u16 k = h.kind();
    if (k == s.font_kind) font_retire(e, s, h);
    else if (k == s.sound_kind) sound_retire(e, s, h);
    else if (k == s.blob_kind) blob_retire(e, s, h);
    else if (k == s.shape_kind) shape_retire(e, s, h);
    else if (k == s.stream_kind) stream_retire(e, s, h);
    else if (const AssetKindDesc* x = external_kind(s, k)) x->retire(e, x->user, h);
    else return;
    ++s.retired;
}

} // namespace

bool asset_scopes_init(Engine&, AssetState_& s, ModuleInstance& self) {
    s.dying.alloc  = &self.general;
    s.graves.alloc = &self.general;
    kv_handle_kind_desc desc{};
    desc.name = "asset.scope"; desc.type_key = type_key<AssetScope_>(); desc.ctx = &s;
    desc.resolve = scope_resolve; desc.name_of = scope_name_of; desc.find_name = scope_find_name; desc.describe = scope_describe; desc.flags = KV_HANDLE_GENERATIONAL;
    s.scope_kind = handle_kind_register(desc);
    if (!s.scope_kind) return false;
    s.scopes.use_kind(s.scope_kind);
    if (!s.scopes.init(KV_ASSET_SCOPES, self.general.tag)) return false;
    AssetScope_ g{};
    utf8_copy(g.name, sizeof g.name, "global");
    g.global = true;
    g.members.alloc = &self.general;
    s.global_scope = s.scopes.create(g);
    return !s.global_scope.is_null();
}

void asset_scopes_shutdown(Engine&, AssetState_& s) {
    for (u32 i = 0; i < s.scopes.len; ++i) {
        if (!s.scopes.is_alive(i)) continue;
        AssetScope_& sc = s.scopes.items[i];
        if (!sc.global) KV_WARN("asset", "scope '%s' still held at shutdown: %u assets", sc.name, sc.members.len);
        sc.members.free();
    }
    for (const AssetGrave& g : s.graves) free_in(s.general, g.ptr, g.bytes);
    if (s.graves.len) KV_INFO("asset", "%u retired sample buffers were still out at shutdown; freed", s.graves.len);
    s.graves.free();
    s.dying.free();
    KV_INFO("asset", "%u assets retired", s.retired);
    kv_handle_kind_unregister(s.scope_kind);
    s.scopes.free();
}

u32* asset_refs_of(AssetState_& s, Handle h) {
    u16 k = h.kind();
    if (k == s.font_kind)          { if (FontAsset* f = s.fonts.get(h))         return &f->refs; }
    else if (k == s.sound_kind)    { if (SoundAsset* d = s.sounds.get(h))       return &d->refs; }
    else if (k == s.blob_kind)     { if (BlobAsset* b = s.blobs.get(h))         return &b->refs; }
    else if (k == s.shape_kind)    { if (ShapeAsset* b = s.shapes.get(h))       return &b->refs; }
    else if (k == s.stream_kind)   { if (StreamAsset* t = s.streams.get(h))     return &t->refs; }
    else if (const AssetKindDesc* x = external_kind(s, k)) return x->refs(x->user, h);
    return nullptr;
}

void asset_ref(AssetState_& s, Handle h) {
    if (u32* r = asset_refs_of(s, h)) ++*r;
}

void asset_unref(AssetState_& s, Handle h) {
    u32* r = asset_refs_of(s, h);
    if (!r || *r == 0) return;
    if (--*r == 0) s.dying.push(h);
}

Handle asset_hold(AssetState_& s, Handle scope, Handle asset) {
    if (asset.is_null()) return asset;
    if (scope.is_null()) { // no scope: the caller holds it with asset_ref, or it goes at the next pump
        if (u32* r = asset_refs_of(s, asset); r && *r == 0) s.dying.push(asset);
        return asset;
    }
    AssetScope_* sc = s.scopes.get(scope);
    if (!sc) { KV_ERROR("asset", "load into a stale scope (%s)", path_of(s, asset)); return Handle{}; }
    for (Handle m : sc->members)
        if (m == asset) return asset;
    if (!asset_refs_of(s, asset)) return Handle{};
    sc->members.push(asset);
    asset_ref(s, asset);
    sc->ready_sent = false;
    return asset;
}

void asset_bury_samples(AssetState_& s, f32* samples, usize bytes) {
    if (!samples) return;
    u64 ticket = s.next_ticket++;
    if (s.samples_hook && s.samples_hook(s.samples_hook_user, ticket, samples)) {
        s.graves.push(AssetGrave{ticket, samples, bytes});
        ++s.deferred;
        return;
    }
    free_in(s.general, samples, bytes);
}

void asset_reap_orphans(Engine& e, AssetState_& s, u16 kind) {
    IoQueue* io = platform_io(e);
    if (!io) return;
    for (u32 i = 0; i < io->completions().len;) {
        IoCompletion c = io->completions()[i];
        if (!asset_is_kind(s, c.user, kind) || asset_refs_of(s, Handle{c.user})) { ++i; continue; }
        io->reap(c.request);
        c.bytes.free();
    }
}

// Phase PreSim, before the other pumps: records nobody holds go, then scopes whose
// members had all settled by the last pump say so (so a scope is reported on the tick
// after its last member turned Ready or Failed).
void asset_scopes_pump(TickContext& ctx) {
    AssetState_& s = ctx.state_as<AssetState_>();
    Engine& e = *ctx.engine;
    // Retiring a material unrefs its texture, which may join the list: run to empty.
    for (u32 i = 0; i < s.dying.len; ++i) {
        Handle h = s.dying[i];
        u32* r = asset_refs_of(s, h);
        if (r && *r == 0) retire(e, s, h); // taken again since it reached zero: it stays
    }
    s.dying.clear();
    for (u32 i = 0; i < s.scopes.len; ++i) {
        if (!s.scopes.is_alive(i)) continue;
        AssetScope_& sc = s.scopes.items[i];
        if (sc.ready_sent || sc.global) continue;
        AssetScopeProgress p = progress_of(s, sc);
        if (p.pending) continue;
        sc.ready_sent = true;
        KV_INFO("asset", "scope '%s' ready: %u assets, %u failed, %llu KB", sc.name, p.members, p.failed, (unsigned long long)(p.bytes / 1024));
        e.events.emit(AssetScopeReady{s.scopes.handle_at(i), p.failed});
    }
}

Handle asset_scope_create(Engine& e, const char* name) {
    AssetState_* s = asset_state(e);
    if (!s) return Handle{};
    AssetScope_ sc{};
    utf8_copy(sc.name, sizeof sc.name, name ? name : "");
    sc.members.alloc = s->general;
    Handle h = s->scopes.create(sc);
    if (h.is_null()) KV_ERROR("asset", "scope '%s': %u scopes are alive", name ? name : "", KV_ASSET_SCOPES);
    return h;
}

Handle asset_scope_global(Engine& e) {
    AssetState_* s = asset_state(e);
    return s ? s->global_scope : Handle{};
}

bool asset_scope_release(Engine& e, Handle scope) {
    AssetState_* s = asset_state(e);
    AssetScope_* sc = s ? s->scopes.get(scope) : nullptr;
    if (!sc) return false;
    if (sc->global) { KV_ERROR("asset", "the global scope is never released"); return false; }
    for (Handle h : sc->members) asset_unref(*s, h);
    sc->members.free();
    s->scopes.destroy(scope);
    return true;
}

bool asset_scope_add(Engine& e, Handle scope, Handle asset) {
    AssetState_* s = asset_state(e);
    if (!s || !s->scopes.get(scope) || !asset_refs_of(*s, asset)) return false;
    return !asset_hold(*s, scope, asset).is_null();
}

AssetScopeProgress asset_scope_progress(Engine& e, Handle scope) {
    AssetState_* s = asset_state(e);
    AssetScope_* sc = s ? s->scopes.get(scope) : nullptr;
    return sc ? progress_of(*s, *sc) : AssetScopeProgress{};
}

const char* asset_scope_name(Engine& e, Handle scope) {
    AssetState_* s = asset_state(e);
    AssetScope_* sc = s ? s->scopes.get(scope) : nullptr;
    return sc ? sc->name : nullptr;
}

u32 asset_refs(Engine& e, Handle asset) {
    AssetState_* s = asset_state(e);
    u32* r = s ? asset_refs_of(*s, asset) : nullptr;
    return r ? *r : 0;
}

bool asset_kind_register(Engine& e, const AssetKindDesc& desc) {
    AssetState_* s = asset_state(e);
    if (!s || !desc.kind || !desc.refs || !desc.state || !desc.retire) return false;
    if (external_kind(*s, desc.kind)) return false;
    if (s->external_count >= KV_ASSET_EXTERNAL_KINDS) { KV_ERROR("asset", "%u external asset kinds already", KV_ASSET_EXTERNAL_KINDS); return false; }
    s->external[s->external_count++] = desc;
    return true;
}

void asset_kind_unregister(Engine& e, u16 kind) {
    AssetState_* s = asset_state(e);
    if (!s) return;
    for (u32 i = 0; i < s->external_count; ++i)
        if (s->external[i].kind == kind) { s->external[i] = s->external[--s->external_count]; return; }
}

Handle asset_hold(Engine& e, Handle scope, Handle asset) {
    AssetState_* s = asset_state(e);
    return s ? asset_hold(*s, scope, asset) : Handle{};
}

void asset_ref(Engine& e, Handle asset) {
    if (AssetState_* s = asset_state(e)) asset_ref(*s, asset);
}

void asset_unref(Engine& e, Handle asset) {
    if (AssetState_* s = asset_state(e)) asset_unref(*s, asset);
}

AssetLifetimeStats asset_lifetime_stats(Engine& e) {
    AssetState_* s = asset_state(e);
    if (!s) return AssetLifetimeStats{};
    u32 live = 0;
    for (u32 i = 0; i < s->scopes.len; ++i) live += s->scopes.is_alive(i) ? 1 : 0;
    return AssetLifetimeStats{s->retired, s->graves.len, s->deferred, s->dying.len, live};
}

void asset_samples_retire_hook(Engine& e, AssetSamplesRetireFn fn, void* user) {
    AssetState_* s = asset_state(e);
    if (!s) return;
    s->samples_hook = fn;
    s->samples_hook_user = user;
}

void asset_retire_done(Engine& e, u64 ticket) {
    AssetState_* s = asset_state(e);
    if (!s) return;
    for (u32 i = 0; i < s->graves.len; ++i) {
        if (s->graves[i].ticket != ticket) continue;
        free_in(s->general, s->graves[i].ptr, s->graves[i].bytes);
        s->graves[i] = s->graves[s->graves.len - 1];
        --s->graves.len;
        return;
    }
}

// ---- the CLI: scope commands ------------------------------------------------------------------

namespace {

Handle scope_named(AssetState_& s, const char* name) {
    for (u32 i = 0; i < s.scopes.len; ++i)
        if (s.scopes.is_alive(i) && strcmp(s.scopes.items[i].name, name) == 0) return s.scopes.handle_at(i);
    return Handle{};
}

bool ends_with(const char* s, const char* suffix) {
    usize n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

void cmd_scope_create(CliContext& ctx, Span<const char* const> args) {
    if (args.len != 1) { ctx.fail("scope create <name>"); return; }
    Handle h = asset_scope_create(*ctx.engine, args[0]);
    if (h.is_null()) { ctx.fail("no room for another scope"); return; }
    ctx.print("scope '%s' created", args[0]);
}

// The kind comes from the extension: .ttf (at 32 px), .wav or .ogg, or the suffix another
// module's kind registered (asset_render's .png and .material.json).
void cmd_scope_load(CliContext& ctx, Span<const char* const> args) {
    Engine& e = *ctx.engine;
    AssetState_* s = asset_state(e);
    if (!s || args.len != 2) { ctx.fail("scope load <scope> <path>"); return; }
    Handle sc = strcmp(args[0], "global") == 0 ? s->global_scope : scope_named(*s, args[0]);
    if (sc.is_null()) { ctx.fail("no scope named '%s'", args[0]); return; }
    const char* path = args[1];
    Handle h;
    const AssetKindDesc* x = nullptr;
    for (u32 i = 0; i < s->external_count && !x; ++i)
        if (s->external[i].load && s->external[i].suffix && ends_with(path, s->external[i].suffix)) x = &s->external[i];
    if (ends_with(path, ".ttf")) h = asset_load_font(e, path, FontDesc{}, sc);
    else if (ends_with(path, ".wav") || ends_with(path, ".ogg")) h = asset_load_sound(e, path, sc);
    else if (x) h = x->load(e, x->user, path, sc);
    else { ctx.fail("unknown kind of asset: %s", path); return; }
    if (h.is_null()) { ctx.fail("could not load '%s'", path); return; }
    ctx.print("%s in '%s', %u refs", path, args[0], asset_refs(e, h));
}

void cmd_scope_release(CliContext& ctx, Span<const char* const> args) {
    AssetState_* s = asset_state(*ctx.engine);
    if (!s || args.len != 1) { ctx.fail("scope release <name>"); return; }
    Handle sc = scope_named(*s, args[0]);
    if (sc.is_null() || !asset_scope_release(*ctx.engine, sc)) { ctx.fail("cannot release '%s'", args[0]); return; }
    ctx.print("scope '%s' released", args[0]);
}

} // namespace

constinit const CliCommandDesc k_asset_commands[4] = {
    {.name = "scope create", .help = "scope create <name>: a new asset scope", .fn = cmd_scope_create},
    {.name = "scope load", .help = "scope load <scope> <path>: load an asset into a scope (global for the global one)", .fn = cmd_scope_load},
    {.name = "scope release", .help = "scope release <name>: release a scope; what nobody holds retires at the next pump", .fn = cmd_scope_release},
    {.name = "reload", .help = "reload [PATH...]: read these assets' files again (all of them with none) and rebind what changed", .fn = asset_cmd_reload},
};

// ---- the CLI: /scopes ---------------------------------------------------------------------------

void asset_scopes_list(CliContext& ctx, const char* dir, CliEmitFn emit, void* user) {
    AssetState_* s = asset_state(*ctx.engine);
    CliTree tree{dir, emit, user};
    for (u32 i = 0; s && i < s->scopes.len; ++i) {
        if (!s->scopes.is_alive(i)) continue;
        AssetScope_& sc = s->scopes.items[i];
        AssetScopeProgress p = progress_of(*s, sc);
        char name[64], summary[128];
        format(name, sizeof name, "%u-%s", i, sc.name);
        format(summary, sizeof summary, "%u assets: %u pending, %u ready, %u failed, %llu KB", p.members, p.pending, p.ready,
                 p.failed, (unsigned long long)(p.bytes / 1024));
        if (!tree.add(name, false, summary)) return;
    }
}

bool asset_scopes_describe(CliContext& ctx, const char* rel) {
    AssetState_* s = asset_state(*ctx.engine);
    for (u32 i = 0; s && i < s->scopes.len; ++i) {
        if (!s->scopes.is_alive(i)) continue;
        AssetScope_& sc = s->scopes.items[i];
        char name[64];
        format(name, sizeof name, "%u-%s", i, sc.name);
        if (strcmp(name, rel) != 0) continue;
        AssetScopeProgress p = progress_of(*s, sc);
        ctx.print("scope '%s'%s: %u assets, %u pending, %u ready, %u failed, %llu KB", sc.name, sc.global ? " (global)" : "",
                  p.members, p.pending, p.ready, p.failed, (unsigned long long)(p.bytes / 1024));
        for (Handle h : sc.members) {
            bool alive;
            AssetState st = read_state(*s, h, &alive);
            u32* r = asset_refs_of(*s, h);
            ctx.print("  %s %s, %u refs: %s", kv_handle_kind_name(h.kind()), !alive ? "stale" : st == AssetState::Ready ? "ready" : st == AssetState::Failed ? "failed" : "pending",
                      r ? *r : 0, path_of(*s, h));
        }
        return true;
    }
    return false;
}


} // namespace kv
