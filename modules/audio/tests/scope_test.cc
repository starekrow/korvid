// Asset lifetimes, end to end on sounds (the one asset kind that needs no renderer): a
// scope's loads become ready and say so; two scopes share an asset; releasing retires
// what nobody holds at the next pump and the path reads again; taking a path back before
// the pump keeps it; a read still out when its asset retires is reaped, not left on the
// queue; a retired sound a voice is playing waits for the mixer before its samples go.
#include "korvid/asset/module.h"
#include "korvid/core/format.h"
#include "korvid/core/time.h"
#include "korvid/asset/scope.h"
#include "korvid/asset/sound.h"
#include "korvid/audio/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/test.h"
#include "korvid/platform/io.h"
#include "korvid/platform/module.h"
#include "korvid/platform_audio/module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

using namespace kv;

struct ScopeSeen { u32 ready; Handle last; u32 failed; };
constinit ScopeSeen g_scope = {};
void on_scope_ready(TickContext&, Span<const AssetScopeReady> rs) {
    for (const AssetScopeReady& r : rs) { ++g_scope.ready; g_scope.last = r.scope; g_scope.failed = r.failed; }
}
constinit const HandlerDesc k_scope_handlers[] = {{.name = ".on_scope_ready", KV_ON(AssetScopeReady, on_scope_ready), .phase = Phase::Sim}};
KV_MODULE_DEFINE(k_scope_t) = {.name = "scope_t", .handlers = k_scope_handlers};

// A short 16-bit mono WAV of `frames` frames at 8 kHz.
bool write_wav(const char* path, u32 frames) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    u32 data = frames * 2, riff = 36 + data, fmt_len = 16, rate = 8000, byte_rate = 16000;
    u16 pcm = 1, ch = 1, align = 2, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt_len, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    for (u32 i = 0; i < frames; ++i) { i16 v = (i16)((i % 40) * 500 - 10000); fwrite(&v, 2, 1, f); }
    return fclose(f) == 0;
}

} // namespace

void test_asset_scopes(kv::TestCtx& c) {
    using namespace kv;
    c.begin("asset_scopes");
    platform_audio_null_device(true);
    const ModuleDesc* const list[] = {&platform_module, &platform_audio_module, &asset_module, &audio_module, &k_scope_t};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    u64 tick = 0;
    auto run = [&](u32 n = 1) { for (u32 i = 0; i < n; ++i) { platform_io(e)->pump(); e.phases.run_tick(e, e.events, 1.0 / 60.0, tick++); } };
    // Ticks until `done`, a millisecond apart, for up to two seconds: a read takes real time,
    // not a number of ticks (io_uring punts a read from tmpfs to a worker thread).
    auto settle = [&](auto done) { for (u32 i = 0; i < 2000 && !done(); ++i) { run(); time_sleep(0.001); } };

    char dir[512];
    // The fixtures go in a scratch directory mounted over the defaults, not beside the
    // executable: an iOS app's bundle is signed and read-only.
    char scratch[400];
    KV_CHECK(c, io_make_temp_dir("korvid-audio-scope-test", scratch, sizeof scratch) && platform_io(e)->mount_dir(scratch));
    format(dir, sizeof dir, "%s/audio_scope_test", scratch);
    io_make_dirs(dir);
    char path[600];
    const char* const names[] = {"a", "b", "c"};
    for (const char* name : names) {
        format(path, sizeof path, "%s/%s.wav", dir, name);
        KV_CHECK(c, write_wav(path, name[0] == 'c' ? 48000 : 400));
    }

    // Two scopes, one shared sound, one missing file.
    g_scope = ScopeSeen{};
    Handle town = asset_scope_create(e, "town"), field = asset_scope_create(e, "field");
    Handle a = asset_load_sound(e, "audio_scope_test/a.wav", town);
    Handle b = asset_load_sound(e, "audio_scope_test/b.wav", town);
    Handle missing = asset_load_sound(e, "audio_scope_test/none.wav", field);
    KV_CHECK(c, asset_load_sound(e, "audio_scope_test/b.wav", field) == b && asset_refs(e, b) == 2 && asset_refs(e, a) == 1);
    KV_CHECK(c, asset_load_sound(e, "audio_scope_test/a.wav", town) == a && asset_refs(e, a) == 1); // held once per scope
    KV_CHECK(c, asset_scope_progress(e, town).members == 2 && asset_scope_progress(e, town).pending == 2);
    settle([&] { return g_scope.ready >= 2; });
    KV_CHECK(c, g_scope.ready == 2);
    AssetScopeProgress tp = asset_scope_progress(e, town), fp = asset_scope_progress(e, field);
    KV_CHECK(c, tp.ready == 2 && tp.pending == 0 && tp.failed == 0 && tp.bytes == 2 * 400 * 4);
    KV_CHECK(c, fp.ready == 1 && fp.failed == 1 && !missing.is_null());
    KV_CHECK(c, strcmp(asset_scope_name(e, town), "town") == 0);

    // Release: a goes at the next pump, b stays for the field.
    KV_CHECK(c, asset_scope_release(e, town) && !asset_scope_release(e, town));
    KV_CHECK(c, asset_sound(e, a) != nullptr && asset_lifetime_stats(e).dying == 1); // not yet
    run();
    KV_CHECK(c, asset_sound(e, a) == nullptr && asset_sound(e, b) != nullptr && asset_refs(e, b) == 1);
    Handle again = asset_scope_create(e, "again");
    Handle a2 = asset_load_sound(e, "audio_scope_test/a.wav", again);
    KV_CHECK(c, !a2.is_null() && a2 != a && asset_sound(e, a2)->state == AssetState::Pending); // read again

    // Taken back before the pump: the field lets go of b and the next scope takes it.
    Handle next = asset_scope_create(e, "next");
    KV_CHECK(c, asset_scope_release(e, field));
    KV_CHECK(c, asset_load_sound(e, "audio_scope_test/b.wav", next) == b);
    run();
    KV_CHECK(c, asset_sound(e, b) != nullptr && asset_sound(e, b)->state == AssetState::Ready && asset_refs(e, b) == 1);
    KV_CHECK(c, asset_sound(e, missing) == nullptr);

    // A read still out when its asset retires: the completion is reaped, the data freed.
    Handle brief = asset_scope_create(e, "brief");
    Handle cc = asset_load_sound(e, "audio_scope_test/c.wav", brief);
    asset_scope_release(e, brief);
    run(4);
    settle([&] { return platform_io(e)->pending() == 0 && platform_io(e)->completions().len == 0; });
    KV_CHECK(c, asset_sound(e, cc) == nullptr && platform_io(e)->completions().len == 0 && platform_io(e)->pending() == 0);

    // Refusals: the global scope, a stale scope, a stale asset.
    KV_CHECK(c, !asset_scope_release(e, asset_scope_global(e)));
    KV_CHECK(c, asset_load_sound(e, "audio_scope_test/a.wav", brief).is_null());
    KV_CHECK(c, !asset_scope_add(e, next, a) && asset_scope_add(e, next, a2) && asset_refs(e, a2) == 2);

    // A sound retired while a voice plays it: with a device the samples wait for the mixer's
    // word; without one they go at once. Either way the voice finishes and nothing is left.
    Handle loopy = asset_scope_create(e, "loopy");
    Handle snd = asset_load_sound(e, "audio_scope_test/c.wav", loopy);
    settle([&] { return asset_sound(e, snd)->state != AssetState::Pending; });
    SoundPlay p;
    p.sound = snd; p.loop = true;
    Handle voice = audio_play(e, p);
    run(2);
    u32 deferred_before = asset_lifetime_stats(e).samples_deferred;
    asset_scope_release(e, loopy);
    run();
    KV_CHECK(c, asset_sound(e, snd) == nullptr);
    KV_CHECK(c, asset_lifetime_stats(e).samples_deferred == deferred_before + (audio_stats(e).device ? 1u : 0u)); // handed to the mixer, not freed
    // The device thread runs in real time: give it up to two seconds.
    for (u32 i = 0; i < 400 && (asset_lifetime_stats(e).samples_out || audio_stats(e).live_voices); ++i) { run(); time_sleep(0.005); }
    KV_CHECK(c, asset_lifetime_stats(e).samples_out == 0 && audio_stats(e).live_voices == 0 && !voice.is_null());

    asset_scope_release(e, again);
    asset_scope_release(e, next);
    run();
    KV_CHECK(c, asset_lifetime_stats(e).scopes == 1 && asset_lifetime_stats(e).dying == 0);
    engine_shutdown(e);
    io_remove_tree(scratch);
}
