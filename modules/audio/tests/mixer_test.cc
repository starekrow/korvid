// The mixer on its own: a voice at pitch 2 lasts half as long; a loop joins without a
// discontinuity; a stop takes effect at the next render; a paused voice holds its
// position and resumes; pan and bus gains land where they should; a stale generation
// is ignored; the sim's rate pitches the voices that pause with it; 64 voices mix and the
// ring survives a producer thread hammering it.
#include "korvid/audio/mixer.h"
#include "korvid/core/test.h"
#include "korvid/math/scalar.h"
#include "korvid/core/os_thread.h"

namespace {

using namespace kv;

bool near(f32 a, f32 b, f32 eps = 1e-3f) { return kv::fabs(a - b) < eps; }
bool near(f64 a, f64 b, f64 eps = 1e-3) { return kv::fabs(a - b) < eps; } // a voice position is f64

MixCmd play_cmd(u32 slot, u32 gen, const f32* samples, u32 frames, u32 rate, u32 channels, f32 pitch = 1.0f, bool loop = false) {
    MixCmd c{};
    c.op = MixOp::Play; c.slot = slot; c.gen = gen; c.bus = (u8)AudioBus::Sfx;
    c.samples = samples; c.frames = frames; c.rate = rate; c.channels = channels;
    c.volume = 1.0f; c.pitch = pitch; c.pan = 0.0f; c.loop = loop; c.pause_with_sim = true;
    return c;
}

u32 drain_done(Mixer& m) { u64 d; u32 n = 0; while (m.done.pop(d)) ++n; return n; }

struct Hammer { SpscRing<u64, 64>* ring; u64 count; };
void hammer(void* user) {
    Hammer& h = *(Hammer*)user;
    for (u64 i = 1; i <= h.count;) if (h.ring->push(i)) ++i;
}

} // namespace

void test_mixer(kv::TestCtx& c) {
    using namespace kv;
    c.begin("mixer");

    // A one-second ramp at 48 kHz mono, and a stereo sound with the channels distinct.
    static f32 ramp[48000];
    for (u32 i = 0; i < 48000; ++i) ramp[i] = (f32)i / 48000.0f;
    static f32 stereo[100 * 2];
    for (u32 i = 0; i < 100; ++i) { stereo[i * 2] = 0.5f; stereo[i * 2 + 1] = -0.25f; }
    static f32 out[1024 * 2];

    Mixer m;
    mixer_init(m, 48000, 2);
    KV_CHECK(c, m.in.push(play_cmd(0, 1, ramp, 48000, 48000, 1)));
    mixer_render(m, out, 1024);
    KV_CHECK(c, m.stats.active == 1 && near(out[0], 0.0f));
    // Center pan is cos(pi/4) on both sides; frame 512 of the ramp is 512/48000.
    KV_CHECK(c, near(out[512 * 2], ramp[512] * 0.7071f) && near(out[512 * 2 + 1], ramp[512] * 0.7071f));
    // 48000 frames at pitch 1: done after 47 renders of 1024 (48128 frames), not before.
    for (u32 i = 0; i < 45; ++i) mixer_render(m, out, 1024);
    KV_CHECK(c, m.stats.active == 1 && drain_done(m) == 0);
    mixer_render(m, out, 1024);
    KV_CHECK(c, m.stats.active == 0 && drain_done(m) == 1);

    // Pitch 2: half the frames. 23.44 renders of 1024, so done on the 24th.
    m.in.push(play_cmd(1, 1, ramp, 48000, 48000, 1, 2.0f));
    for (u32 i = 0; i < 23; ++i) mixer_render(m, out, 1024);
    KV_CHECK(c, m.stats.active == 1);
    mixer_render(m, out, 1024);
    KV_CHECK(c, m.stats.active == 0 && drain_done(m) == 1);
    // A 24 kHz sound at pitch 1 reads every other output frame: it also lasts two seconds.
    m.in.push(play_cmd(1, 2, ramp, 48000, 24000, 1));
    for (u32 i = 0; i < 93; ++i) mixer_render(m, out, 1024);
    KV_CHECK(c, m.stats.active == 1);
    mixer_render(m, out, 1024);
    KV_CHECK(c, m.stats.active == 0 && drain_done(m) == 1);

    // A loop: at the seam the sample after the last is the first (the ramp drops to 0),
    // and it never finishes.
    m.in.push(play_cmd(2, 1, ramp, 48000, 48000, 1, 1.0f, true));
    for (u32 i = 0; i < 46; ++i) mixer_render(m, out, 1024);
    mixer_render(m, out, 1024); // frames 47104..48127: the wrap is at output frame 896 of this render
    KV_CHECK(c, m.stats.active == 1 && drain_done(m) == 0);
    KV_CHECK(c, near(out[895 * 2] / 0.7071f, ramp[47999]) && near(out[896 * 2] / 0.7071f, ramp[0]));
    for (u32 i = 0; i < 100; ++i) mixer_render(m, out, 1024);
    KV_CHECK(c, m.stats.active == 1);

    // Stop: silent from the next render; the slot reports done once. A Set or Stop with
    // the wrong generation is ignored.
    MixCmd stop{}; stop.op = MixOp::Stop; stop.slot = 2; stop.gen = 7;
    m.in.push(stop);
    mixer_render(m, out, 1024);
    KV_CHECK(c, m.stats.active == 1);
    stop.gen = 1;
    m.in.push(stop);
    mixer_render(m, out, 1024);
    KV_CHECK(c, m.stats.active == 0 && drain_done(m) == 1 && near(out[0], 0.0f));

    // Pause with the sim: the voice holds its position and resumes where it was; one
    // that does not pause with the sim plays through.
    MixCmd a = play_cmd(3, 1, ramp, 48000, 48000, 1);
    MixCmd b = play_cmd(4, 1, ramp, 48000, 48000, 1);
    b.pause_with_sim = false;
    b.pan = -1.0f; // hard left
    m.in.push(a); m.in.push(b);
    mixer_render(m, out, 1024);
    MixCmd pause{}; pause.op = MixOp::PauseSim;
    m.in.push(pause);
    mixer_render(m, out, 1024);
    KV_CHECK(c, near(m.voices[3].pos, 1024.0) && near(m.voices[4].pos, 2048.0));
    KV_CHECK(c, near(out[0 * 2 + 1], 0.0f) && near(out[10 * 2 + 1], 0.0f) && out[0 * 2] > 0.0f && out[10 * 2] > 0.0f); // b alone, left only
    MixCmd resume{}; resume.op = MixOp::ResumeSim;
    m.in.push(resume);
    mixer_render(m, out, 1024);
    KV_CHECK(c, near(m.voices[3].pos, 2048.0));

    // Bus gains: Sfx at 0.5 and Master at 0.5 quarter the output; a Set changes volume.
    mixer_init(m, 48000, 2);
    MixCmd bus{}; bus.op = MixOp::Bus; bus.bus = (u8)AudioBus::Sfx; bus.volume = 0.5f;
    MixCmd master{}; master.op = MixOp::Bus; master.bus = (u8)AudioBus::Master; master.volume = 0.5f;
    m.in.push(bus); m.in.push(master);
    m.in.push(play_cmd(0, 1, stereo, 100, 48000, 2, 1.0f, true));
    mixer_render(m, out, 64);
    KV_CHECK(c, near(out[0], 0.5f * 0.25f * 0.7071f) && near(out[1], -0.25f * 0.25f * 0.7071f));
    MixCmd set{}; set.op = MixOp::Set; set.slot = 0; set.gen = 1; set.volume = 2.0f; set.pitch = 1.0f; set.pan = 1.0f;
    m.in.push(set);
    mixer_render(m, out, 64);
    KV_CHECK(c, near(out[0], 0.0f) && near(out[1], -0.25f * 0.5f)); // hard right, volume 2 x 0.25

    // Clipping is counted and clamped.
    mixer_init(m, 48000, 2);
    static f32 loud[100];
    for (f32& v : loud) v = 1.0f;
    for (u32 i = 0; i < 4; ++i) m.in.push(play_cmd(i, 1, loud, 100, 48000, 1, 1.0f, true));
    mixer_render(m, out, 64);
    KV_CHECK(c, m.stats.clipped == 128 && near(out[0], 1.0f));

    // Every voice at once, and a mono output.
    mixer_init(m, 48000, 1);
    for (u32 i = 0; i < KV_AUDIO_VOICES; ++i) m.in.push(play_cmd(i, 1, ramp, 48000, 48000, 1, 1.0f + 0.01f * (f32)i));
    mixer_render(m, out, 1024);
    KV_CHECK(c, m.stats.active == KV_AUDIO_VOICES && m.stats.peak_active == KV_AUDIO_VOICES);
    // Slot 64 is out of range: ignored, not a crash.
    m.in.push(play_cmd(KV_AUDIO_VOICES, 1, ramp, 48000, 48000, 1));
    mixer_render(m, out, 16);

    // Retire: every voice on the samples stops and reports, then the acknowledgement comes,
    // last, with the ticket; voices on other samples play on.
    mixer_init(m, 48000, 2);
    drain_done(m);
    m.in.push(play_cmd(0, 3, ramp, 48000, 48000, 1, 1.0f, true));
    m.in.push(play_cmd(1, 4, ramp, 48000, 48000, 1, 1.0f, true));
    m.in.push(play_cmd(2, 5, stereo, 100, 48000, 2, 1.0f, true));
    mixer_render(m, out, 64);
    MixCmd rc{};
    rc.op = MixOp::Retire; rc.samples = ramp; rc.gen = 77; rc.slot = 0xFFFFFFFFu;
    m.in.push(rc);
    mixer_render(m, out, 64);
    u64 d[4]; u32 nd = 0;
    while (nd < 4 && m.done.pop(d[nd])) ++nd;
    KV_CHECK(c, nd == 3 && d[0] == (0 | (3ull << 32)) && d[1] == (1 | (4ull << 32)) && d[2] == (KV_MIX_RETIRED | (77ull << 32)));
    KV_CHECK(c, m.stats.active == 1 && !m.voices[0].active && m.voices[2].active);
    // A full done ring keeps the acknowledgement and delivers it once there is room.
    u32 filled = 0;
    while (m.done.push(1)) ++filled;
    rc.gen = 78;
    m.in.push(rc);
    mixer_render(m, out, 16);
    KV_CHECK(c, m.ack_count == 1);
    drain_done(m);
    mixer_render(m, out, 16);
    KV_CHECK(c, m.ack_count == 0 && m.done.pop(d[0]) && d[0] == (KV_MIX_RETIRED | (78ull << 32)) && filled > 0);

    // The sim's rate: a voice that pauses with the sim plays at the time scale's pitch (the
    // game floors it at half); one that plays through does not.
    {
        Mixer r;
        mixer_init(r, 48000, 2);
        MixCmd rate{};
        rate.op = MixOp::SimRate;
        rate.volume = 0.5f;
        MixCmd through = play_cmd(1, 1, ramp, 48000, 48000, 1);
        through.pause_with_sim = false;
        r.in.push(rate);
        r.in.push(play_cmd(0, 1, ramp, 48000, 48000, 1));
        r.in.push(through);
        mixer_render(r, out, 1024);
        KV_CHECK(c, r.sim_rate == 0.5f && near(r.voices[0].pos, 512.0) && near(r.voices[1].pos, 1024.0));
        rate.volume = 1.0f;
        r.in.push(rate);
        mixer_render(r, out, 1024);
        KV_CHECK(c, near(r.voices[0].pos, 1536.0) && near(r.voices[1].pos, 2048.0));
    }

    // The ring under a producer thread: every value arrives, in order, none twice.
    SpscRing<u64, 64> ring;
    Hammer h{&ring, 200000};
    OsThread t;
    KV_CHECK(c, os_thread_start(&t, hammer, &h));
    u64 expect = 1, v, bad = 0;
    while (expect <= h.count) { if (ring.pop(v)) { if (v != expect) ++bad; ++expect; } }
    os_thread_join(&t);
    KV_CHECK(c, bad == 0 && !ring.pop(v));
}
