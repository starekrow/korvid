// The module without a device (none in a test): voices are handed out and finish on the
// next tick with VoiceFinished; a stop before the start finishes without playing; the
// pool recycles slots with new generations; a full pool gives up its lowest-priority,
// oldest voice or refuses; a sound asked for twice in a tick is one start; a play with an
// unknown sound finishes; the command path checks the declared write.
#include "korvid/audio/module.h"
#include "korvid/asset/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/test.h"
#include "korvid/platform/module.h"
#include "korvid/platform_audio/module.h"
#include <stdlib.h>

namespace {

using namespace kv;

struct Seen { u32 finished; Handle last; };
constinit Seen g = {};
void on_finished(TickContext&, Span<const VoiceFinished> fs) { for (const VoiceFinished& f : fs) { ++g.finished; g.last = f.voice; } }
constinit const HandlerDesc k_handlers[] = {{.name = ".on_finished", KV_ON(VoiceFinished, on_finished)}};

constinit Handle g_voice = {};
constinit bool   g_live_after = false;
void sys_play(TickContext& ctx) {
    static bool once = false;
    if (once) return;
    once = true;
    SoundPlay p;
    p.sound = Handle{}; // no such sound: finishes at once either way
    g_voice = audio_play(ctx, p);
    g_live_after = audio_voice_live(ctx, g_voice);
}
constinit const SystemDesc k_systems[] = {{.name = ".play", .phase = Phase::Sim, .fn = sys_play, .writes = "audio.commands"}};
KV_MODULE_DEFINE(k_t) = {.name = "t", .systems = k_systems, .handlers = k_handlers};

} // namespace

void test_audio_module(kv::TestCtx& c) {
    using namespace kv;
    c.begin("audio_module");

    // The audio device module is up (SDL audio on a dummy driver) so the audio module can try
    // the device; on a machine without one it runs silent, which is what this test wants
    // either way: voices on a null sound finish at the next extract.
    platform_audio_null_device(true);
    const ModuleDesc* const list[] = {&platform_module, &platform_audio_module, &asset_module, &audio_module, &k_t};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    u64 tick = 0;
    auto run = [&]() { e.phases.run_tick(e, e.events, 1.0 / 60.0, tick++); };

    g = Seen{};
    run(); // t.play asks; audio.extract finishes it (no sound); the event is pending
    KV_CHECK(c, !g_voice.is_null() && g_live_after);
    KV_CHECK(c, audio_stats(e).live_voices == 0);
    run();
    KV_CHECK(c, g.finished == 1 && g.last == g_voice);

    // From outside the schedule: 64 voices, then none free. The 65th, of the same priority,
    // takes the oldest (whose handle goes stale and finishes at the extract); one of a lower
    // priority than every voice is refused. A stop before the start.
    SoundPlay p;
    Handle first = audio_play(e, p);
    for (u32 i = 1; i < KV_AUDIO_VOICES; ++i) KV_CHECK(c, !audio_play(e, p).is_null());
    KV_CHECK(c, audio_stats(e).live_voices == KV_AUDIO_VOICES);
    Handle taker = audio_play(e, p);
    KV_CHECK(c, !taker.is_null() && taker.index() == first.index() && taker.gen() != first.gen() && audio_stats(e).steals == 1);
    KV_CHECK(c, !audio_stop(e, first) && audio_stats(e).live_voices == KV_AUDIO_VOICES);
    SoundPlay low = p;
    low.priority = 0;
    KV_CHECK(c, audio_play(e, low).is_null() && audio_stats(e).dropped == 1);
    KV_CHECK(c, audio_stop(e, taker));
    g = Seen{};
    run(); run();
    KV_CHECK(c, g.finished == KV_AUDIO_VOICES + 1 && audio_stats(e).live_voices == 0); // the 64, and the voice taken
    KV_CHECK(c, !audio_stop(e, first)); // stale now
    Handle again = audio_play(e, p);
    KV_CHECK(c, again.index() == first.index() && again.gen() != first.gen());
    audio_bus_volume(e, AudioBus::Music, 0.5f);
    run(); run();
    KV_CHECK(c, audio_stats(e).live_voices == 0 && audio_stats(e).dropped == 1); // the low-priority play was the drop

    // One start a sound a tick: a second play of a sound before the extract is the first's
    // voice; the next tick's is a voice of its own.
    SoundPlay a;
    a.sound = Handle::make(1, 1, 7); // no such asset: it finishes at the extract, which is all this needs
    a.volume = 0.3f;
    Handle once = audio_play(e, a);
    a.volume = 0.8f;
    a.priority = 0;
    KV_CHECK(c, audio_play(e, a) == once && audio_stats(e).merged == 1 && audio_stats(e).live_voices == 1);
    run();
    Handle next = audio_play(e, a);
    KV_CHECK(c, !next.is_null() && next != once && audio_stats(e).merged == 1);
    run(); run();

    engine_shutdown(e);
}
