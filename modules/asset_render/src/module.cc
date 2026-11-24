// Korvid asset_render: the module record. Textures and materials, the asset kinds that are the
// renderer's, registered with the asset module; and the glyph pages of its fonts, drawn.
#include "korvid/asset_render/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/platform/io.h"
#include "asset_render_internal.h"

namespace kv {

AssetRenderState* asset_render_state(Engine& e) { return engine_module_state_as<AssetRenderState>(e, "asset_render"); }

void asset_render_reap_orphans(Engine& e, AssetRenderState& s, u16 kind) {
    IoQueue* io = platform_io(e);
    if (!io) return;
    for (u32 i = 0; i < io->completions().len;) {
        IoCompletion c = io->completions()[i];
        Handle h{c.user};
        bool gone = kind != 0 && h.kind() == kind && (kind == s.kind ? !s.textures.get(h) : !s.materials.get(h));
        if (!gone) { ++i; continue; }
        io->reap(c.request);
        c.bytes.free();
    }
}

namespace {

bool startup(Engine& e, ModuleInstance& self) {
    AssetRenderState* s = arena_push<AssetRenderState>(self.arena);
    if (!s) return false;
    s->general = &self.general;
    self.set_state(s);
    if (!asset_textures_init(e, *s, self)) return false;
    if (!asset_materials_init(e, *s, self)) return false;
    asset_render_fonts_init(e);
    return true;
}

void shutdown(Engine& e, ModuleInstance& self) {
    AssetRenderState* s = self.state_as<AssetRenderState>();
    if (!s) return;
    asset_render_fonts_shutdown(e);
    // Reads still out (a load cut short by the exit) are the module's to collect: cancel
    // what has not started, let the rest finish, and free what any of them brought.
    if (IoQueue* io = platform_io(e)) {
        auto ours = [&](u64 user) { u16 k = Handle{user}.kind(); return k != 0 && (k == s->kind || k == s->material_kind); };
        for (u32 round = 0; round < 1000; ++round) {
            for (u32 i = 0; i < io->completions().len;) {
                IoCompletion c = io->completions()[i];
                if (!ours(c.user)) { ++i; continue; }
                io->reap(c.request);
                c.bytes.free();
            }
            if (!io->pending()) break;
            io->pump();
        }
    }
    asset_materials_shutdown(e, *s);
    asset_textures_shutdown(e, *s);
}

constinit const ResourceDesc k_resources[] = {{".textures"}, {".materials"}};
constinit const SystemDesc   k_systems[]   = {
    // After the asset module's scopes, which retire what nobody holds (through the kinds'
    // retire), so the pumps see this tick's tables.
    {.name = ".pump", .phase = Phase::PreSim, .fn = asset_textures_pump, .writes = ".textures",
     .after = "asset.pump_scopes", .main_thread = true, .runs = RunWhen::Always},
    {.name = ".pump_materials", .phase = Phase::PreSim, .fn = asset_materials_pump, .reads = ".textures",
     .writes = ".materials", .after = ".pump", .main_thread = true, .runs = RunWhen::Always},
};

} // namespace

KV_MODULE_DEFINE(asset_render_module) = {
    .name      = "asset_render",
    .resources = k_resources,
    .systems   = k_systems,
    .mounts    = k_asset_render_mounts,
    .startup   = startup,
    .shutdown  = shutdown,
    .memory    = {.reserve = 16 * 1024},
};

} // namespace kv
