// Korvid asset: the module record. The state every kind lives in, its startup and shutdown,
// and the systems: the scopes' pump first, then each kind's.
#include "korvid/asset/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/platform/io.h"
#include "asset_internal.h"

namespace kv {

AssetState_* asset_state(Engine& e) { return engine_module_state_as<AssetState_>(e, "asset"); }

namespace {

const char* k_error_names[(u32)AssetError::Count] = {"none", "not found", "read error", "decode error", "upload error", "parse error"};

bool asset_startup(Engine& e, ModuleInstance& self) {
    AssetState_* s = arena_push<AssetState_>(self.arena);
    if (!s) return false;
    s->general = &self.general;
    if (!asset_fonts_init(e, *s, self)) return false;
    if (!asset_sounds_init(e, *s, self)) return false;
    if (!asset_blobs_init(e, *s, self)) return false;
    if (!asset_shapes_init(e, *s, self)) return false;
    if (!asset_streams_init(e, *s, self)) return false;
    if (!asset_scopes_init(e, *s, self)) return false;
    self.set_state(s);
    return true;
}

void asset_shutdown(Engine& e, ModuleInstance& self) {
    AssetState_* s = self.state_as<AssetState_>();
    if (!s) return;
    // Reads still out (a load cut short by the exit) are the module's to collect: what has
    // not started is cancelled, the rest finish, and what any of them brought is freed.
    if (IoQueue* io = platform_io(e)) {
        const u16 kinds[] = {s->font_kind, s->sound_kind, s->blob_kind, s->stream_kind, s->shape_kind};
        for (u16 k : kinds) io->drain(asset_io_owner(k));
    }
    asset_scopes_shutdown(e, *s);
    asset_streams_shutdown(e, *s);
    asset_shapes_shutdown(e, *s);
    asset_blobs_shutdown(e, *s);
    asset_sounds_shutdown(e, *s);
    asset_fonts_shutdown(e, *s);
}

constinit const EventTypeDesc k_events[]    = {KV_EVENT(AssetReady), KV_EVENT(AssetFailed), KV_EVENT(AssetScopeReady), KV_EVENT(AssetReloaded)};
constinit const ResourceDesc  k_resources[] = {{".fonts"}, {".sounds"}, {".blobs"}, {".streams"}, {".shapes"}};
constinit const SystemDesc    k_systems[]   = {
    // Retiring first: it writes every table (another module's kinds' too, through their
    // retire), and the other pumps then see this tick's state.
    {.name = ".pump_scopes", .phase = Phase::PreSim, .fn = asset_scopes_pump, .writes = ".fonts, .sounds, .blobs, .streams, .shapes",
     .main_thread = true, .runs = RunWhen::Always},
    {.name = ".pump_fonts", .phase = Phase::PreSim, .fn = asset_fonts_pump, .writes = ".fonts",
     .after = ".pump_scopes", .main_thread = true, .runs = RunWhen::Always},
    // The Frame cycle: glyph pages the frame's text added to, uploaded before it draws.
    {.name = ".fonts_flush", .phase = Phase::Frame, .fn = asset_fonts_flush,
     .after = "?krome.paint, ?sprite_text.layout, ?sprite_ui.draw", .main_thread = true, .runs = RunWhen::Always},
    {.name = ".pump_sounds", .phase = Phase::PreSim, .fn = asset_sounds_pump, .writes = ".sounds",
     .after = ".pump_scopes", .main_thread = true, .runs = RunWhen::Always},
    {.name = ".pump_blobs", .phase = Phase::PreSim, .fn = asset_blobs_pump, .writes = ".blobs",
     .after = ".pump_scopes", .main_thread = true, .runs = RunWhen::Always},
    {.name = ".pump_streams", .phase = Phase::PreSim, .fn = asset_streams_pump, .writes = ".streams",
     .after = ".pump_scopes", .main_thread = true, .runs = RunWhen::Always},
    {.name = ".pump_shapes", .phase = Phase::PreSim, .fn = asset_shapes_pump, .writes = ".shapes",
     .after = ".pump_scopes", .main_thread = true, .runs = RunWhen::Always},
};

} // namespace

KV_MODULE_DEFINE(asset_module) = {
    .name      = "asset",
    .events    = k_events,
    .resources = k_resources,
    .systems   = k_systems,
    .commands  = k_asset_commands,
    .mounts    = k_asset_mounts,
    .startup   = asset_startup,
    .shutdown  = asset_shutdown,
    .memory    = {.reserve = 64 * 1024},
};

const char* asset_error_name(AssetError err) {
    return (u32)err < (u32)AssetError::Count ? k_error_names[(u32)err] : "?";
}

} // namespace kv
