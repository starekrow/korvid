// Fades and streams. The mixer: a linear fade ends on its target on the right frame; an
// equal-power crossfade keeps the summed power flat; a fade that stops finishes the voice;
// a stream voice plays what its ring holds, counts an underrun as silence, and finishes
// when the ring has ended and drained. The module, over arcanum's vendored track when the
// build has staged it: a stream decodes, music_play starts it, the same track again is a
// no-op, another track crossfades, and every decoder and ring is gone at the end.
#include "korvid/audio/audio.h"
#include "korvid/audio/module.h"
#include "korvid/asset/module.h"
#include "korvid/asset/scope.h"
#include "korvid/asset/stream.h"
#include "korvid/core/engine.h"
#include "korvid/core/test.h"
#include "korvid/platform/io.h"
#include "korvid/platform/module.h"
#include "korvid/platform_audio/module.h"
#include <math.h>
#include <stdlib.h>
#include <unistd.h>

namespace {

using namespace kv;
bool near(f32 a, f32 b, f32 eps = 1e-3f) { return fabsf(a - b) < eps; }

MixCmd play(u32 slot, const f32* samples, u32 frames, f32 volume, bool loop) {
    MixCmd c{};
    c.op = MixOp::Play; c.slot = slot; c.gen = 1; c.bus = (u8)AudioBus::Music;
    c.samples = samples; c.frames = frames; c.rate = 48000; c.channels = 1;
    c.volume = volume; c.pitch = 1.0f; c.loop = loop; c.pause_with_sim = false;
    return c;
}
MixCmd fade(u32 slot, f32 to, f32 seconds, FadeCurve curve, bool stop) {
    MixCmd c{};
    c.op = MixOp::Fade; c.slot = slot; c.gen = 1; c.volume = to; c.fade_seconds = seconds; c.curve = curve; c.stop_at_end = stop;
    return c;
}

} // namespace

void test_fades(kv::TestCtx& c) {
    using namespace kv;
    c.begin("fades");
    static f32 ones[48000];
    for (f32& v : ones) v = 1.0f;
    static f32 out[480 * 2];
    Mixer m;
    mixer_init(m, 48000, 2);
    const f32 center = 0.70710678f; // the pan law at center
    // Linear: 0 to 1 over 0.01 s (480 frames). Frame k plays at volume k / 480: the step
    // after it is what moves the volume.
    m.in.push(play(0, ones, 48000, 0.0f, true));
    m.in.push(fade(0, 1.0f, 0.01f, FadeCurve::Linear, false));
    mixer_render(m, out, 480);
    KV_CHECK(c, near(out[0], 0.0f) && near(out[240 * 2], center * 0.5f) && near(out[479 * 2], center * 479.0f / 480.0f));
    mixer_render(m, out, 16);
    KV_CHECK(c, near(out[0], center) && near(m.voices[0].volume, 1.0f));
    // Equal power: one voice out, one in, over the same 480 frames: l^2 + r^2 per frame of
    // the summed volumes squared stays at 1 (two voices of one signal: (v0^2 + v1^2) is 1).
    m.in.push(play(1, ones, 48000, 0.0f, true));
    m.in.push(fade(0, 0.0f, 0.01f, FadeCurve::EqualPower, true));
    m.in.push(fade(1, 1.0f, 0.01f, FadeCurve::EqualPower, false));
    f32 worst = 0.0f;
    for (u32 k = 0; k < 480; ++k) {
        mixer_render(m, out, 1);
        f32 v0 = m.voices[0].active ? m.voices[0].volume : 0.0f, v1 = m.voices[1].volume;
        f32 p = v0 * v0 + v1 * v1;
        if (fabsf(p - 1.0f) > worst) worst = fabsf(p - 1.0f);
    }
    KV_CHECK(c, worst < 1e-3f);
    // The fade that stopped finished its voice and said so.
    u64 d, done0 = 0;
    while (m.done.pop(d)) if ((d & 0xFFFFFFFFu) == 0) ++done0;
    KV_CHECK(c, !m.voices[0].active && done0 == 1 && m.voices[1].active && near(m.voices[1].volume, 1.0f));

    // A stream voice: a ring of 64 frames holding 40 of a ramp, at the device's rate.
    mixer_init(m, 48000, 2);
    static f32 ring_frames[64 * 2];
    StreamRing ring;
    ring.frames = ring_frames; ring.capacity = 64;
    for (u32 i = 0; i < 40; ++i) { ring_frames[i * 2] = (f32)i / 100.0f; ring_frames[i * 2 + 1] = -(f32)i / 100.0f; }
    ring.write = 40;
    MixCmd sp{};
    sp.op = MixOp::Play; sp.slot = 3; sp.gen = 7; sp.ring = &ring; sp.rate = 48000; sp.channels = 2;
    sp.volume = 1.0f; sp.pitch = 1.0f; sp.pan = 0.0f;
    m.in.push(sp);
    mixer_render(m, out, 16);
    KV_CHECK(c, near(out[5 * 2], 0.05f * center) && near(out[5 * 2 + 1], -0.05f * center) && ring.read == 16);
    // 30 more frames wanted, 24 there (and one held back for interpolation): an underrun.
    mixer_render(m, out, 30);
    KV_CHECK(c, ring.underruns == 1 && m.voices[3].active && ring.read == 39);
    // The producer ends the stream: the voice drains and finishes.
    __atomic_store_n(&ring.ended, true, __ATOMIC_RELEASE);
    mixer_render(m, out, 30);
    KV_CHECK(c, !m.voices[3].active && m.done.pop(d) && d == (3u | (7ull << 32)));

    // Loop points from the comments.
    const char* const tags[] = {"TITLE=x", "LOOPSTART=1000", "LOOPLENGTH=5000"};
    u64 s0 = 0, e0 = 0;
    KV_CHECK(c, stream_loop_from_comments(tags, 3, 100000, &s0, &e0) && s0 == 1000 && e0 == 6000);
    const char* const tags2[] = {"loopstart=10", "LOOPEND=99999999"};
    KV_CHECK(c, stream_loop_from_comments(tags2, 2, 5000, &s0, &e0) && s0 == 10 && e0 == 5000);
    KV_CHECK(c, !stream_loop_from_comments(tags, 1, 100000, &s0, &e0) && s0 == 0 && e0 == 100000);
}

void test_music(kv::TestCtx& c) {
    using namespace kv;
    c.begin("music");
    setenv("SDL_AUDIO_DRIVER", "dummy", 0);
    const ModuleDesc* const list[] = {&platform_module, &platform_audio_module, &asset_module, &audio_module};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    auto frames = [&](u32 n) { for (u32 i = 0; i < n; ++i) { platform_pump(e); engine_frame(e, 1.0 / 60.0); usleep(2000); } };
    Handle scope = asset_scope_create(e, "music");
    Handle village = asset_load_stream(e, "example_arcanum/music/village.ogg", scope);
    Handle road = asset_load_stream(e, "example_arcanum/music/road.ogg", scope);
    for (u32 i = 0; i < 100 && asset_stream(e, road)->state == AssetState::Pending; ++i) frames(1);
    if (asset_stream(e, village)->state != AssetState::Ready) {
        KV_INFO("test", "music: arcanum's tracks are not staged in this build; the module part is skipped");
        engine_shutdown(e);
        return;
    }
    const StreamAsset* v = asset_stream(e, village);
    KV_CHECK(c, v->rate == 44100 && v->channels == 2 && v->frames > 44100 * 40 && v->loop_end == v->frames);
    // A decoder over it gives the frames and seeks.
    static u8 mem[KV_STREAM_DECODER_MEMORY];
    static f32 pcm[4096 * 2];
    StreamDecoder* d = stream_decoder_open(*v, mem, sizeof mem);
    KV_CHECK(c, d && stream_decoder_read(d, pcm, 4096) == 4096 && stream_decoder_seek(d, v->frames - 100) && stream_decoder_read(d, pcm, 4096) == 100);
    stream_decoder_close(d);

    MusicPlay p;
    p.fade_out = 0.2f; p.fade_in = 0.2f; p.overlap = 0.2f;
    KV_CHECK(c, music_play(e, village, p) && music_current(e) == village);
    frames(10);
    AudioStats st = audio_stats(e);
    bool device = st.device;
    KV_CHECK(c, !device || st.stream_voices == 1);
    KV_CHECK(c, music_play(e, village, p) && audio_stats(e).stream_voices == st.stream_voices); // the same track: nothing
    KV_CHECK(c, music_play(e, road, p) && music_current(e) == road);
    frames(4);
    KV_CHECK(c, !device || audio_stats(e).stream_voices == 2); // both, crossfading
    for (u32 i = 0; i < 400 && audio_stats(e).stream_voices > 1; ++i) frames(1); // a fifth of a second of the device's real time
    KV_CHECK(c, !device || audio_stats(e).stream_voices == 1);
    KV_CHECK(c, audio_stats(e).underruns == 0);
    // While a voice plays the stream, releasing its scope does not retire it.
    asset_scope_release(e, scope);
    frames(2);
    KV_CHECK(c, asset_stream(e, road) != nullptr || !device);
    music_stop(e, 0.05f);
    for (u32 i = 0; i < 400 && audio_stats(e).stream_voices; ++i) frames(1);
    KV_CHECK(c, audio_stats(e).stream_voices == 0 && music_current(e).is_null());
    frames(2);
    KV_CHECK(c, asset_stream(e, road) == nullptr && asset_stream(e, village) == nullptr);
    engine_shutdown(e);
}
