#include "korvid/audio/mixer.h"
#include <math.h>
#include <string.h>

namespace kv {

void mixer_init(Mixer& m, u32 out_rate, u32 out_channels) {
    for (MixVoice& v : m.voices) v = MixVoice{};
    for (f32& g : m.bus_gain) g = 1.0f;
    m.sim_paused   = false;
    m.out_rate     = out_rate ? out_rate : 48000;
    m.out_channels = out_channels ? out_channels : 2;
    m.stats = MixerStats{};
}

namespace {

void apply(Mixer& m, const MixCmd& c) {
    switch (c.op) {
    case MixOp::Play: {
        if (c.slot >= KV_AUDIO_VOICES) return;
        MixVoice& v = m.voices[c.slot];
        v.samples = c.samples; v.frames = c.frames; v.rate = c.rate; v.channels = c.channels;
        v.pos = 0.0; v.volume = c.volume; v.pitch = c.pitch; v.pan = c.pan;
        v.bus = c.bus; v.loop = c.loop; v.pause_with_sim = c.pause_with_sim;
        v.gen = c.gen;
        v.active = c.samples && c.frames && c.rate && c.channels;
        if (!v.active) m.done.push((u64)c.slot | ((u64)c.gen << 32));
        break;
    }
    case MixOp::Set: {
        if (c.slot >= KV_AUDIO_VOICES) return;
        MixVoice& v = m.voices[c.slot];
        if (!v.active || v.gen != c.gen) return;
        v.volume = c.volume; v.pitch = c.pitch; v.pan = c.pan;
        break;
    }
    case MixOp::Stop: {
        if (c.slot >= KV_AUDIO_VOICES) return;
        MixVoice& v = m.voices[c.slot];
        if (!v.active || v.gen != c.gen) return;
        v.active = false;
        m.done.push((u64)c.slot | ((u64)c.gen << 32));
        break;
    }
    case MixOp::PauseSim:  m.sim_paused = true; break;
    case MixOp::ResumeSim: m.sim_paused = false; break;
    case MixOp::Bus:
        if (c.bus < (u32)AudioBus::Count) m.bus_gain[c.bus] = c.volume;
        break;
    case MixOp::Retire: {
        for (u32 i = 0; i < KV_AUDIO_VOICES; ++i) {
            MixVoice& v = m.voices[i];
            if (!v.active || v.samples != c.samples) continue;
            v.active = false;
            m.done.push((u64)i | ((u64)v.gen << 32));
        }
        u64 ack = KV_MIX_RETIRED | ((u64)c.gen << 32);
        if (!m.done.push(ack) && m.ack_count < KV_COUNTOF(m.acks)) m.acks[m.ack_count++] = ack;
        break;
    }
    }
}

// One voice into the output: linear interpolation at the rate ratio times the pitch,
// equal-power pan, the voice's volume times its bus and the master.
void mix_voice(Mixer& m, MixVoice& v, f32* out, u32 frames) {
    f64 step = (f64)v.rate / (f64)m.out_rate * (f64)(v.pitch > 0.0f ? v.pitch : 0.0f);
    if (step <= 0.0) return; // pitch 0: held
    f32 gain = v.volume * m.bus_gain[v.bus] * m.bus_gain[(u32)AudioBus::Master];
    f32 theta = (v.pan + 1.0f) * 0.25f * 3.14159265f;
    f32 gl = gain * cosf(theta), gr = gain * sinf(theta);
    u32 oc = m.out_channels, sc = v.channels;
    for (u32 i = 0; i < frames; ++i) {
        u32 i0 = (u32)v.pos;
        if (i0 >= v.frames) break;
        f32 frac = (f32)(v.pos - (f64)i0);
        u32 i1 = i0 + 1;
        if (i1 >= v.frames) i1 = v.loop ? 0 : i0;
        f32 l, r;
        if (sc == 1) {
            l = r = v.samples[i0] + (v.samples[i1] - v.samples[i0]) * frac;
        } else if (sc == 2) {
            l = v.samples[i0 * 2] + (v.samples[i1 * 2] - v.samples[i0 * 2]) * frac;
            r = v.samples[i0 * 2 + 1] + (v.samples[i1 * 2 + 1] - v.samples[i0 * 2 + 1]) * frac;
        } else {
            f32 a = 0.0f, b = 0.0f;
            for (u32 ch = 0; ch < sc; ++ch) { a += v.samples[i0 * sc + ch]; b += v.samples[i1 * sc + ch]; }
            l = r = (a + (b - a) * frac) / (f32)sc;
        }
        if (oc == 1) out[i] += (l + r) * 0.5f * gain;
        else { out[i * oc] += l * gl; out[i * oc + 1] += r * gr; }
        v.pos += step;
        if (v.pos >= (f64)v.frames) {
            if (v.loop) v.pos = fmod(v.pos, (f64)v.frames);
            else { v.active = false; m.done.push((u64)(&v - m.voices) | ((u64)v.gen << 32)); break; }
        }
    }
}

} // namespace

void mixer_render(Mixer& m, f32* out, u32 frames) {
    while (m.ack_count && m.done.push(m.acks[0])) { // oldest first; the ring has room again
        for (u32 i = 1; i < m.ack_count; ++i) m.acks[i - 1] = m.acks[i];
        --m.ack_count;
    }
    MixCmd c;
    while (m.in.pop(c)) apply(m, c);
    memset(out, 0, (usize)frames * m.out_channels * sizeof(f32));
    u32 active = 0;
    for (MixVoice& v : m.voices) {
        if (!v.active) continue;
        if (v.pause_with_sim && m.sim_paused) { ++active; continue; } // held where it is
        mix_voice(m, v, out, frames);
        if (v.active) ++active;
    }
    u32 clipped = 0;
    for (usize i = 0; i < (usize)frames * m.out_channels; ++i) {
        if (out[i] > 1.0f) { out[i] = 1.0f; ++clipped; }
        else if (out[i] < -1.0f) { out[i] = -1.0f; ++clipped; }
    }
    m.stats.frames_rendered += frames;
    m.stats.callbacks += 1;
    m.stats.active = active;
    if (active > m.stats.peak_active) m.stats.peak_active = active;
    m.stats.clipped += clipped;
}

} // namespace kv
