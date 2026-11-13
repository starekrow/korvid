#include "korvid/audio/module.h"
#include "korvid/core/format.h"
#include "korvid/core/atomic.h"
#include "korvid/asset/scope.h"
#include "korvid/asset/sound.h"
#include "korvid/asset/stream.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/profile.h"
#include "korvid/platform_audio/audio.h"
#include "korvid/core/os_thread.h"
#include <stdlib.h>
#include <string.h>

namespace kv {

namespace {

// A voice as the game sees it: what to play, and whether the mixer has it yet.
struct VoiceRec {
    SoundPlay play;
    u32       gen     = 0;     // the handle's generation; bumped when the slot is freed
    bool      used    = false;
    bool      started = false; // the Play command went to the mixer
    bool      stopped = false; // a stop was asked before the start: finish without playing
    // A stream voice: the decoder (in dec_mem) runs ahead of the mixer into `ring`.
    bool           stream = false;
    StreamRing     ring;
    void*          dec_mem = nullptr;
    StreamDecoder* dec = nullptr;
    u64            dec_pos = 0;      // the stream frame the decoder gives next
    // A fade asked for before the voice started: sent right after the start.
    bool      fade_pending = false;
    f32       fade_to = 0.0f, fade_seconds = 0.0f;
    FadeCurve fade_curve = FadeCurve::EqualPower;
    bool      fade_stop = false;
    u64       order = 0;       // when it was asked for: the oldest of a priority is stolen first
    u32       batch = 0;       // the extract it waits for: a second play of its sound before it is one start
    // The handles callers hold for it: its own from outside the schedule, a reservation
    // from a system's play, more than one when plays merged. Each gets its VoiceFinished.
    Handle    given[8];
    u32       ngiven = 0;
};

// A system's play gets a reservation at once, numbered like a ticket by its unit, and the
// voice behind it at the next boundary, in unit order. This bit in the index marks one.
constexpr u32 kReserved = 0x80000000u;

constexpr u32 kStreamRingFrames = 1u << 16; // about 1.4 s at 44.1 kHz, 512 KB stereo

// A command the game queued this tick, applied in order by the extract.
struct Queued {
    MixOp     op;
    Handle    voice;
    f32       volume, pitch, pan;
    u8        bus;
    f32       seconds = 0.0f;   // Fade
    FadeCurve curve = FadeCurve::EqualPower;
    bool      stop = false;
};

// The music: the track playing (or fading in), and one waiting for its turn in a crossfade.
struct Music {
    Handle    stream, voice;
    Handle    next;            // starts when `delay` runs out
    f32       delay = 0.0f;
    MusicPlay next_play;
    bool      waiting = false;
};

// Samples the asset module is retiring: sent to the mixer by the extract, freed when it
// acknowledges.
struct Retiring { u64 ticket; const f32* samples; bool sent; };

struct AudioCommands {
    VoiceRec        voices[KV_AUDIO_VOICES];
    Array<Queued>   queued;
    Array<Retiring> retiring;
    OsMutex mu;
    u32             dropped = 0;
    u32             steals = 0, merged = 0;
    u64             next_order = 0;
    u32             batch = 0;     // +1 at every extract
    Array<Handle>   stolen;        // voices taken by a play: their VoiceFinished goes out at the extract
};

// What a system asks for, as an append record: applied at the boundary, in unit order.
enum class ReqOp : u8 { Play, Queue, Fade, Music };
struct AudioRequest {
    ReqOp     op;
    MixOp     mix = MixOp::Set; // Queue: Set, Stop or Bus
    Handle    voice;            // Play: the reservation; Queue and Fade: as the caller holds it; Music: the stream
    SoundPlay play;
    MusicPlay music;
    f32       volume = 0, pitch = 0, pan = 0, seconds = 0;
    u8        bus = 0;
    FadeCurve curve = FadeCurve::EqualPower;
    bool      stop = false;
};

struct AudioState {
    Engine*       engine = nullptr;
    u32           commands = 0;     // the id of "audio.commands"
    Array<u32>    reserved;         // by unit (0: outside the schedule): its reservations so far
    Array<u64>    pending;          // reservations made and not applied yet
    Music         music;
    Allocator*    general = nullptr;
    Mixer         mixer;
    AudioCommands cmds;
    u16           kind = 0;
    bool          device = false;
    bool          last_paused = false;
    f32           last_rate = 1.0f;
    u32           dropped_logged = 0;
};

AudioState* state(Engine& e) { return engine_module_state_as<AudioState>(e, "audio"); }

struct Lock {
    OsMutex& mu;
    explicit Lock(OsMutex& m) : mu(m) { os_mutex_lock(&mu); }
    ~Lock() { os_mutex_unlock(&mu); }
};

// A voice's own handle, or a reservation it was given.
void* voice_resolve(kv_handle h, void* ctx) {
    AudioCommands& c = *(AudioCommands*)ctx;
    Handle hh{h};
    u32 i = hh.index();
    if (i & kReserved) {
        for (VoiceRec& v : c.voices)
            if (v.used)
                for (u32 k = 0; k < v.ngiven; ++k)
                    if (v.given[k] == hh) return &v;
        return nullptr;
    }
    if (i >= KV_AUDIO_VOICES || !c.voices[i].used || c.voices[i].gen != hh.gen()) return nullptr;
    return &c.voices[i];
}

// The voice's own handle, from any handle to it.
Handle voice_own(const AudioCommands& c, const VoiceRec* v, u16 kind) { return Handle::make(kind, v->gen, (u32)(v - c.voices)); }

// Every handle a caller holds for the voice in `slot`: what its VoiceFinished goes to.
void push_given(Array<Handle>& to, const VoiceRec& v) {
    for (u32 k = 0; k < v.ngiven; ++k) to.push(v.given[k]);
}
int voice_describe(kv_handle h, void* ctx, char* out, size_t cap) {
    VoiceRec* v = (VoiceRec*)voice_resolve(h, ctx);
    if (!v) return format(out, cap, "stale");
    return format(out, cap, "%s vol %.2f pitch %.2f%s", v->started ? "playing" : "waiting", v->play.volume, v->play.pitch, v->play.loop ? " loop" : "");
}

// The device thread.
void device_callback(void* user, f32* out, u32 frames) { mixer_render(*(Mixer*)user, out, frames); }

void free_voice(AudioState& s, u32 slot);

// The lock held: a voice from the pool. A stream (asset.stream) makes a stream voice,
// which holds a reference on it until it is done. A sound already starting this tick is
// the same start; a full pool gives up its lowest-priority, oldest voice, or refuses.
// `reserved` is the handle a system's play was given; null for a play from outside, which
// gets the voice's own.
Handle play_locked(AudioState& s, const SoundPlay& play, Handle reserved = Handle{}) {
    if (!play.sound.is_null()) {
        for (u32 i = 0; i < KV_AUDIO_VOICES; ++i) {
            VoiceRec& v = s.cmds.voices[i];
            if (!v.used || v.started || v.stopped || v.stream || v.batch != s.cmds.batch || v.play.sound != play.sound || v.play.bus != play.bus ||
                v.play.loop != play.loop || v.ngiven == KV_COUNTOF(v.given))
                continue;
            u8 prio = v.play.priority > play.priority ? v.play.priority : play.priority;
            if (play.volume > v.play.volume) v.play = play; // the louder wins
            v.play.priority = prio;
            ++s.cmds.merged;
            Handle h = reserved.is_null() ? Handle::make(s.kind, v.gen, i) : reserved;
            bool known = false;
            for (u32 k = 0; k < v.ngiven; ++k) known = known || v.given[k] == h;
            if (!known) v.given[v.ngiven++] = h;
            return h;
        }
    }
    u32 slot = KV_AUDIO_VOICES;
    for (u32 i = 0; i < KV_AUDIO_VOICES && slot == KV_AUDIO_VOICES; ++i)
        if (!s.cmds.voices[i].used) slot = i;
    if (slot == KV_AUDIO_VOICES) {
        // Every voice busy: the lowest priority's oldest goes, if it is not above this one.
        for (u32 i = 0; i < KV_AUDIO_VOICES; ++i) {
            const VoiceRec& v = s.cmds.voices[i];
            if (v.stream) continue;
            if (slot == KV_AUDIO_VOICES || v.play.priority < s.cmds.voices[slot].play.priority ||
                (v.play.priority == s.cmds.voices[slot].play.priority && v.order < s.cmds.voices[slot].order))
                slot = i;
        }
        if (slot == KV_AUDIO_VOICES || s.cmds.voices[slot].play.priority > play.priority) {
            ++s.cmds.dropped;
            return Handle{};
        }
        // Its start, if it had one, is overwritten in the mixer by this one's (same slot,
        // new generation); anything still queued for it no longer resolves.
        push_given(s.cmds.stolen, s.cmds.voices[slot]);
        free_voice(s, slot);
        ++s.cmds.steals;
    }
    {
        u32 i = slot;
        VoiceRec& v = s.cmds.voices[i];
        v.used = true;
        v.order = s.cmds.next_order++;
        v.batch = s.cmds.batch;
        v.started = v.stopped = false;
        v.play = play;
        v.stream = s.engine && asset_stream(*s.engine, play.sound) != nullptr;
        if (v.stream) asset_ref(*s.engine, play.sound);
        if (v.gen == 0 || v.gen == 0xFFFFF) v.gen = 1; else ++v.gen;
        v.given[0] = reserved.is_null() ? Handle::make(s.kind, v.gen, i) : reserved;
        v.ngiven = 1;
        return v.given[0];
    }
}
Handle play_in(AudioState& s, const SoundPlay& play) {
    Lock lock(s.cmds.mu);
    return play_locked(s, play);
}
bool queue_locked(AudioState& s, Queued q) {
    if (q.op != MixOp::Bus) {
        const VoiceRec* v = (const VoiceRec*)voice_resolve(q.voice.bits, &s.cmds);
        if (!v) return false;
        q.voice = voice_own(s.cmds, v, s.kind); // the extract works by slot
    }
    s.cmds.queued.push(q);
    return true;
}
bool queue_in(AudioState& s, MixOp op, Handle voice, f32 volume, f32 pitch, f32 pan, u8 bus) {
    Lock lock(s.cmds.mu);
    return queue_locked(s, Queued{op, voice, volume, pitch, pan, bus});
}
bool fade_locked(AudioState& s, Handle voice, f32 to, f32 seconds, FadeCurve curve, bool stop) {
    Queued q{MixOp::Fade, voice, to, 0, 0, 0};
    q.seconds = seconds; q.curve = curve; q.stop = stop;
    return queue_locked(s, q);
}

// The lock held: crossfade to `stream` (null: to silence). See music_play.
bool music_locked(AudioState& s, Handle stream, const MusicPlay& p) {
    Music& m = s.music;
    bool current_live = !m.voice.is_null() && voice_resolve(m.voice.bits, &s.cmds);
    if (!stream.is_null() && m.stream == stream && (current_live || m.waiting)) return true; // already playing it
    f32 delay = 0.0f;
    if (current_live) {
        fade_locked(s, m.voice, 0.0f, p.fade_out, FadeCurve::EqualPower, true);
        delay = p.fade_out - p.overlap;
        if (delay < 0.0f) delay = 0.0f;
    }
    m.voice = Handle{};
    m.stream = stream;
    m.next = stream;
    m.next_play = p;
    m.delay = delay;
    m.waiting = !stream.is_null();
    return true;
}

// A voice's slot back to the pool; a stream voice's decoder, ring and reference go with it
// (the mixer has said it is done, so nothing on the device thread reads the ring).
void free_voice(AudioState& s, u32 slot) {
    VoiceRec& v = s.cmds.voices[slot];
    if (v.stream) {
        if (v.dec) stream_decoder_close(v.dec);
        free_in(s.general, v.dec_mem, KV_STREAM_DECODER_MEMORY);
        free_in(s.general, v.ring.frames, sizeof(f32) * 2 * kStreamRingFrames);
        if (s.engine) asset_unref(*s.engine, v.play.sound);
        v.dec = nullptr; v.dec_mem = nullptr; v.ring = StreamRing{}; v.stream = false;
    }
    v.used = false;
    v.started = false;
    v.fade_pending = false;
    v.ngiven = 0;
}

// Decodes ahead until the ring is full, looping at the stream's loop points; at the end of
// a stream that does not loop, marks the ring ended.
void fill_stream(const StreamAsset& st, VoiceRec& v) {
    StreamRing& r = v.ring;
    static f32 block[2 * 2048];
    for (;;) {
        u32 room = r.capacity - r.available();
        if (room < 256 || r.ended) return;
        u32 want = room < 2048 ? room : 2048;
        u64 limit = v.play.loop ? st.loop_end : st.frames;
        if (v.dec_pos + want > limit) want = (u32)(limit - v.dec_pos);
        u32 got = want ? stream_decoder_read(v.dec, block, want) : 0;
        if (got == 0) {
            if (v.play.loop && stream_decoder_seek(v.dec, st.loop_start)) { v.dec_pos = st.loop_start; continue; }
            kv::atomic_store(&r.ended, true, kv::MO_RELEASE);
            return;
        }
        u64 w = r.write;
        u32 mask = r.capacity - 1;
        for (u32 k = 0; k < got; ++k) {
            f32* dst = r.frames + ((w + k) & mask) * 2;
            dst[0] = block[k * 2]; dst[1] = block[k * 2 + 1];
        }
        kv::atomic_store(&r.write, w + got, kv::MO_RELEASE);
        v.dec_pos += got;
    }
}

// The Play command of a stream voice, its decoder opened and its ring primed. False if the
// stream will not decode.
bool start_stream(AudioState& s, VoiceRec& v, u32 slot, const StreamAsset& st, MixCmd* out) {
    v.dec_mem = alloc_in(s.general, KV_STREAM_DECODER_MEMORY);
    v.ring = StreamRing{};
    v.ring.frames = (f32*)alloc_in(s.general, sizeof(f32) * 2 * kStreamRingFrames);
    v.ring.capacity = kStreamRingFrames;
    v.dec = v.dec_mem ? stream_decoder_open(st, v.dec_mem, KV_STREAM_DECODER_MEMORY) : nullptr;
    if (!v.dec || !v.ring.frames) return false;
    v.dec_pos = 0;
    fill_stream(st, v);
    MixCmd c{};
    c.op = MixOp::Play; c.slot = slot; c.gen = v.gen; c.bus = (u8)v.play.bus;
    c.ring = &v.ring; c.rate = st.rate; c.channels = 2;
    c.volume = v.play.volume; c.pitch = v.play.pitch; c.pan = v.play.pan;
    c.loop = v.play.loop; c.pause_with_sim = v.play.pause_with_sim;
    *out = c;
    return true;
}

MixCmd cmd_for(const VoiceRec& v, u32 slot, const SoundAsset* snd) {
    MixCmd c{};
    c.op = MixOp::Play; c.slot = slot; c.gen = v.gen; c.bus = (u8)v.play.bus;
    c.samples = snd->samples; c.frames = snd->frames; c.rate = snd->rate; c.channels = snd->channels;
    c.volume = v.play.volume; c.pitch = v.play.pitch; c.pan = v.play.pan;
    c.loop = v.play.loop; c.pause_with_sim = v.play.pause_with_sim;
    return c;
}

// Phase PostSim, main thread, every tick: what finished comes back as events, the sim's
// pause reaches the mixer, waiting voices start when their sound is ready, and the
// tick's commands cross in order.
void audio_extract(TickContext& ctx) {
    KV_ZONE("audio.extract");
    AudioState& s = ctx.state_as<AudioState>();
    Engine& e = *ctx.engine;
    Lock lock(s.cmds.mu);

    u64 done;
    while (s.mixer.done.pop(done)) {
        u32 slot = (u32)(done & 0xFFFFFFFFu), gen = (u32)(done >> 32);
        if ((done & 0xFFFFFFFFull) == KV_MIX_RETIRED) {
            for (u32 i = 0; i < s.cmds.retiring.len; ++i) {
                if ((u32)s.cmds.retiring[i].ticket != gen) continue;
                asset_retire_done(e, s.cmds.retiring[i].ticket);
                s.cmds.retiring[i] = s.cmds.retiring[s.cmds.retiring.len - 1];
                --s.cmds.retiring.len;
                break;
            }
            continue;
        }
        VoiceRec& v = s.cmds.voices[slot];
        if (!v.used || v.gen != gen) continue;
        for (u32 k = 0; k < v.ngiven; ++k) ctx.emit(VoiceFinished{v.given[k]});
        free_voice(s, slot);
    }

    for (Retiring& r : s.cmds.retiring) {
        if (r.sent) continue;
        MixCmd c{};
        c.op = MixOp::Retire; c.samples = r.samples; c.gen = (u32)r.ticket; c.slot = 0xFFFFFFFFu;
        if (s.mixer.in.push(c)) r.sent = true; // else tried again next tick
    }

    for (Handle h : s.cmds.stolen) ctx.emit(VoiceFinished{h});
    s.cmds.stolen.clear();

    if (ctx.paused != s.last_paused) {
        s.last_paused = ctx.paused;
        MixCmd c{};
        c.op = ctx.paused ? MixOp::PauseSim : MixOp::ResumeSim;
        if (!s.mixer.in.push(c)) ++s.cmds.dropped;
    }
    // The time scale, as a pitch on the voices that pause with the sim, floored at half.
    f32 rate = (f32)engine_time_scale_get(e);
    rate = rate < 0.5f ? 0.5f : rate > 4.0f ? 4.0f : rate;
    if (rate != s.last_rate) {
        MixCmd c{};
        c.op = MixOp::SimRate;
        c.volume = rate;
        if (s.mixer.in.push(c)) s.last_rate = rate;
        else ++s.cmds.dropped;
    }

    // The music's next track, when its turn in a crossfade comes.
    if (s.music.waiting) {
        s.music.delay -= (f32)ctx.dt;
        if (s.music.delay <= 0.0f) {
            SoundPlay p;
            p.sound = s.music.next; p.volume = 0.0f; p.loop = true; p.pause_with_sim = false; p.bus = AudioBus::Music; p.priority = 3;
            s.music.voice = play_locked(s, p);
            if (!s.music.voice.is_null()) fade_locked(s, s.music.voice, s.music.next_play.volume, s.music.next_play.fade_in, FadeCurve::EqualPower, false);
            s.music.waiting = false;
        }
    }

    for (u32 i = 0; i < KV_AUDIO_VOICES; ++i) {
        VoiceRec& v = s.cmds.voices[i];
        if (!v.used || v.started) continue;
        MixCmd start{};
        bool finish, ready;
        if (v.stream) {
            const StreamAsset* st = asset_stream(e, v.play.sound);
            finish = v.stopped || !st || st->state == AssetState::Failed || !s.device;
            ready  = !finish && st->state == AssetState::Ready;
            if (ready && !start_stream(s, v, i, *st, &start)) { KV_WARN("audio", "stream '%s' will not decode", st->path); finish = true; ready = false; }
        } else {
            const SoundAsset* snd = asset_sound(e, v.play.sound);
            finish = v.stopped || !snd || snd->state == AssetState::Failed || !s.device;
            ready  = !finish && snd->state == AssetState::Ready;
            if (ready) start = cmd_for(v, i, snd);
        }
        if (finish) {
            for (u32 k = 0; k < v.ngiven; ++k) ctx.emit(VoiceFinished{v.given[k]});
            free_voice(s, i);
            continue;
        }
        if (!ready) continue; // waits for the file
        if (!s.mixer.in.push(start)) { ++s.cmds.dropped; continue; }
        v.started = true;
        if (v.fade_pending) {
            MixCmd f{};
            f.op = MixOp::Fade; f.slot = i; f.gen = v.gen; f.volume = v.fade_to;
            f.fade_seconds = v.fade_seconds; f.curve = v.fade_curve; f.stop_at_end = v.fade_stop;
            if (!s.mixer.in.push(f)) ++s.cmds.dropped;
            v.fade_pending = false;
        }
    }

    for (const Queued& q : s.cmds.queued) {
        if (q.op == MixOp::Bus) {
            MixCmd c{};
            c.op = MixOp::Bus; c.bus = q.bus; c.volume = q.volume;
            if (!s.mixer.in.push(c)) ++s.cmds.dropped;
            continue;
        }
        VoiceRec* v = (VoiceRec*)voice_resolve(q.voice.bits, &s.cmds);
        if (!v) continue;
        u32 slot = q.voice.index();
        if (q.op == MixOp::Set) {
            v->play.volume = q.volume; v->play.pitch = q.pitch; v->play.pan = q.pan;
            if (!v->started) continue; // the start will carry them
        } else if (q.op == MixOp::Fade) {
            if (!v->started) { // sent right after the start
                v->fade_pending = true; v->fade_to = q.volume; v->fade_seconds = q.seconds; v->fade_curve = q.curve; v->fade_stop = q.stop;
                continue;
            }
        } else if (!v->started) { v->stopped = true; continue; } // finishes next tick, never plays
        MixCmd c{};
        c.op = q.op; c.slot = slot; c.gen = v->gen;
        c.volume = q.volume; c.pitch = q.pitch; c.pan = q.pan;
        c.fade_seconds = q.seconds; c.curve = q.curve; c.stop_at_end = q.stop;
        if (!s.mixer.in.push(c)) ++s.cmds.dropped;
    }
    s.cmds.queued.clear();

    if (s.cmds.dropped != s.dropped_logged) {
        KV_WARN("audio", "%u commands dropped (ring full or no voice to take)", s.cmds.dropped - s.dropped_logged);
        s.dropped_logged = s.cmds.dropped;
    }
    ++s.cmds.batch; // the next plays are the next tick's
}

// The asset module is about to free a sound's samples: keep them until the mixer has
// stopped every voice on them. Called on the main thread, from the asset pump.
bool retire_samples(void* user, u64 ticket, const f32* samples) {
    AudioState& s = *(AudioState*)user;
    if (!s.device) return false; // nothing reads samples without a device thread: free now
    Lock lock(s.cmds.mu);
    s.cmds.retiring.push(Retiring{ticket, samples, false});
    return true;
}

bool audio_startup(Engine& e, ModuleInstance& self) {
    AudioState* s = arena_push<AudioState>(self.arena);
    if (!s) return false;
    os_mutex_init(&s->cmds.mu);
    s->cmds.queued.alloc = &self.general;
    s->cmds.retiring.alloc = &self.general;
    s->cmds.stolen.alloc = &self.general;
    s->engine  = &e;
    s->general = &self.general;
    kv_handle_kind_desc desc{};
    desc.name = "audio.voice"; desc.type_key = type_key<VoiceRec>(); desc.ctx = &s->cmds;
    desc.resolve = voice_resolve; desc.describe = voice_describe; desc.flags = KV_HANDLE_GENERATIONAL;
    s->kind = handle_kind_register(desc);
    if (!s->kind) return false;
    if (!engine_publish(e, self, ".commands", &s->cmds)) return false;
    s->commands = engine_resource_id(e, "audio.commands");
    s->reserved.alloc = s->pending.alloc = &self.general;
    s->reserved.resize_fill(e.units + 1, 0);
    self.set_state(s);

    AudioDeviceDesc dd;
    dd.rate = 48000; dd.channels = 2; dd.frames = 512;
    dd.callback = device_callback; dd.user = &s->mixer;
    mixer_init(s->mixer, dd.rate, dd.channels);
    s->device = platform_audio_open(e, dd);
    if (!s->device) KV_WARN("audio", "no device: running silent, voices finish at once");
    asset_samples_retire_hook(e, retire_samples, s);
    return true;
}

void audio_shutdown(Engine& e, ModuleInstance& self) {
    AudioState* s = self.state_as<AudioState>();
    if (!s) return;
    platform_audio_close(e); // the callbacks have stopped when this returns
    asset_samples_retire_hook(e, nullptr, nullptr);
    for (const Retiring& r : s->cmds.retiring) asset_retire_done(e, r.ticket); // nothing reads them now
    for (u32 i = 0; i < KV_AUDIO_VOICES; ++i) if (s->cmds.voices[i].used) free_voice(*s, i); // stream decoders and rings
    s->cmds.retiring.free();
    KV_INFO("audio", "%llu frames rendered in %llu callbacks, peak %u voices, %u clipped samples, %u commands dropped",
            (unsigned long long)s->mixer.stats.frames_rendered, (unsigned long long)s->mixer.stats.callbacks,
            s->mixer.stats.peak_active, s->mixer.stats.clipped, s->cmds.dropped);
    kv_handle_kind_unregister(s->kind);
    s->cmds.queued.free();
    s->cmds.stolen.free();
    s->reserved.free();
    s->pending.free();
    os_mutex_destroy(&s->cmds.mu);
}

// /voices: the slots in use, from the game's side; /buses: the gains.
void voices_list(CliContext& ctx, const char* dir, CliEmitFn emit, void* user) {
    if (dir[0]) return;
    AudioState* s = state(*ctx.engine);
    if (!s) return;
    Lock lock(s->cmds.mu);
    for (u32 i = 0; i < KV_AUDIO_VOICES; ++i) {
        const VoiceRec& v = s->cmds.voices[i];
        if (!v.used) continue;
        char name[16], summary[256], sound[KV_ASSET_PATH_MAX] = "?";
        format(name, sizeof name, "%u", i);
        if (const SoundAsset* snd = asset_sound(*ctx.engine, v.play.sound)) format(sound, sizeof sound, "%s", snd->path);
        format(summary, sizeof summary, "%s %s, vol %.2f, pitch %.2f, pan %.2f%s%s", v.started ? "playing" : "waiting", sound, v.play.volume, v.play.pitch, v.play.pan,
                 v.play.loop ? ", loop" : "", v.play.pause_with_sim ? "" : ", plays through pause");
        if (!emit(user, name, summary)) return;
    }
}

bool voices_describe(CliContext& ctx, const char* rel) {
    AudioState* s = state(*ctx.engine);
    if (!s) return false;
    if (!rel[0]) {
        AudioStats st = audio_stats(*ctx.engine);
        ctx.print("%s; %u live voices, %u dropped commands; mixer: %llu frames in %llu callbacks, %u active, peak %u, %u clipped",
                  st.device ? "device open" : "no device (silent)", st.live_voices, st.dropped, (unsigned long long)st.mixer.frames_rendered,
                  (unsigned long long)st.mixer.callbacks, st.mixer.active, st.mixer.peak_active, st.mixer.clipped);
        return true;
    }
    char* end = nullptr;
    unsigned long slot = strtoul(rel, &end, 10);
    if (end == rel || *end || slot >= KV_AUDIO_VOICES) return false;
    Lock lock(s->cmds.mu);
    const VoiceRec& v = s->cmds.voices[slot];
    if (!v.used) return false;
    char text[200];
    kv_handle_describe(Handle::make(s->kind, v.gen, (u32)slot).bits, text, sizeof text);
    ctx.print("voice %lu: %s, bus %u, loop %s, pauses with sim %s", slot, text, (u32)v.play.bus, v.play.loop ? "yes" : "no", v.play.pause_with_sim ? "yes" : "no");
    return true;
}

const char* bus_name(u32 b) { static const char* k[] = {"master", "sfx", "music", "ui"}; return b < 4 ? k[b] : "?"; }

void buses_list(CliContext& ctx, const char* dir, CliEmitFn emit, void* user) {
    if (dir[0]) return;
    AudioState* s = state(*ctx.engine);
    if (!s) return;
    for (u32 b = 0; b < (u32)AudioBus::Count; ++b) {
        char summary[32];
        format(summary, sizeof summary, "%.2f", s->mixer.bus_gain[b]); // a snapshot: the device thread owns it
        if (!emit(user, bus_name(b), summary)) return;
    }
}

bool buses_describe(CliContext& ctx, const char* rel) {
    AudioState* s = state(*ctx.engine);
    if (!s) return false;
    for (u32 b = 0; b < (u32)AudioBus::Count; ++b)
        if (!rel[0] || strcmp(bus_name(b), rel) == 0) ctx.print("%s %.2f", bus_name(b), s->mixer.bus_gain[b]);
    if (rel[0]) { for (u32 b = 0; b < (u32)AudioBus::Count; ++b) if (strcmp(bus_name(b), rel) == 0) return true; return false; }
    return true;
}

bool buses_patch(CliContext& ctx, const char* rel, Span<const char* const> values) {
    for (u32 b = 0; b < (u32)AudioBus::Count; ++b) {
        if (strcmp(bus_name(b), rel) != 0) continue;
        f32 v;
        if (values.len != 1 || !cli_arg_f32(ctx, values[0], "volume", &v)) { if (!ctx.failed) ctx.fail("one volume"); return false; }
        audio_bus_volume(*ctx.engine, (AudioBus)b, v);
        ctx.print("%s -> %.2f at the next tick", bus_name(b), v);
        return true;
    }
    ctx.fail("no bus '%s'", rel);
    return false;
}

constinit const CliMountDesc k_mounts[] = {
    {.path = "/voices", .list = voices_list, .describe = voices_describe},
    {.path = "/buses",  .list = buses_list,  .describe = buses_describe, .patch = buses_patch},
};

constinit const EventTypeDesc k_events[]    = {KV_EVENT(VoiceFinished)};
// A system's request, at the boundary, in unit order: what the call would have done then
// and there. A play refused (every voice busy with higher priorities) finishes at once.
void audio_apply(Engine& e, const void* record, u32 size) {
    AudioState* s = state(e);
    if (!s || size != sizeof(AudioRequest)) return;
    AudioRequest q;
    memcpy(&q, record, sizeof q);
    Lock lock(s->cmds.mu);
    switch (q.op) {
    case ReqOp::Play:
        for (u32 k = 0; k < s->pending.len; ++k)
            if (s->pending[k] == q.voice.bits) { s->pending[k] = s->pending[s->pending.len - 1]; --s->pending.len; break; }
        if (play_locked(*s, q.play, q.voice).is_null()) s->cmds.stolen.push(q.voice);
        break;
    case ReqOp::Queue: queue_locked(*s, Queued{q.mix, q.voice, q.volume, q.pitch, q.pan, q.bus}); break;
    case ReqOp::Fade:  fade_locked(*s, q.voice, q.volume, q.seconds, q.curve, q.stop); break;
    case ReqOp::Music: music_locked(*s, q.voice, q.music); break;
    }
}

constinit const ResourceDesc  k_resources[] = {{".commands", audio_apply}};
// Phase PostSim, main thread, every tick, after the extract: each stream voice's ring is
// topped up from its decoder, so the mixer always has a second or so ahead of it.
void audio_stream_fill(TickContext& ctx) {
    KV_ZONE("audio.stream_fill");
    AudioState& s = ctx.state_as<AudioState>();
    Lock lock(s.cmds.mu);
    for (VoiceRec& v : s.cmds.voices) {
        if (!v.used || !v.started || !v.stream || !v.dec) continue;
        if (const StreamAsset* st = asset_stream(*ctx.engine, v.play.sound)) fill_stream(*st, v);
    }
}

constinit const SystemDesc    k_systems[] = {
    {.name = ".extract", .phase = Phase::PostSim, .fn = audio_extract, .writes = ".commands",
     .emits = "VoiceFinished", .main_thread = true, .runs = RunWhen::Always},
    {.name = ".stream_fill", .phase = Phase::PostSim, .fn = audio_stream_fill, .writes = ".commands",
     .after = ".extract", .main_thread = true, .runs = RunWhen::Always},
};

} // namespace

KV_MODULE_DEFINE(audio_module) = {
    .name      = "audio",
    .events    = k_events,
    .resources = k_resources,
    .systems   = k_systems,
    .mounts    = k_mounts,
    .startup   = audio_startup,
    .shutdown  = audio_shutdown,
    .memory    = {.reserve = 256 * 1024},
};

// ---- the API ------------------------------------------------------------------------------------

namespace {
// A system's call: it must declare `appends` on audio.commands. The id is per engine.
AudioState* checked(TickContext& ctx) {
    AudioState* s = state(*ctx.engine);
    return s && ctx.may_append(s->commands) ? s : nullptr;
}
bool live_locked(AudioState& s, Handle h) {
    if (voice_resolve(h.bits, &s.cmds)) return true;
    for (u64 p : s.pending) if (p == h.bits) return true;
    return false;
}
bool live(AudioState& s, Handle h) { Lock lock(s.cmds.mu); return live_locked(s, h); }
} // namespace

// From a system, every call is a request applied at the boundary, in unit order (see
// audio_apply); a play's handle is a reservation, live from the call on.
Handle audio_play(TickContext& ctx, const SoundPlay& play) {
    AudioState* s = checked(ctx);
    if (!s) return Handle{};
    u32 unit = ctx.system ? ctx.system->unit : 0;
    u32 n = ++s->reserved[unit] & 0xFFFFFu; // the unit's own counter: no one else touches it
    Handle h = Handle::make(s->kind, n ? n : 1, kReserved | unit);
    { Lock lock(s->cmds.mu); s->pending.push(h.bits); }
    AudioRequest q{};
    q.op = ReqOp::Play; q.voice = h; q.play = play;
    ctx.append(s->commands, q);
    return h;
}
bool audio_set(TickContext& ctx, Handle voice, f32 volume, f32 pitch, f32 pan) {
    AudioState* s = checked(ctx);
    if (!s || !live(*s, voice)) return false;
    AudioRequest q{};
    q.op = ReqOp::Queue; q.mix = MixOp::Set; q.voice = voice; q.volume = volume; q.pitch = pitch; q.pan = pan;
    return ctx.append(s->commands, q);
}
bool audio_stop(TickContext& ctx, Handle voice) {
    AudioState* s = checked(ctx);
    if (!s || !live(*s, voice)) return false;
    AudioRequest q{};
    q.op = ReqOp::Queue; q.mix = MixOp::Stop; q.voice = voice;
    return ctx.append(s->commands, q);
}
void audio_bus_volume(TickContext& ctx, AudioBus bus, f32 volume) {
    AudioState* s = checked(ctx);
    if (!s) return;
    AudioRequest q{};
    q.op = ReqOp::Queue; q.mix = MixOp::Bus; q.volume = volume; q.bus = (u8)bus;
    ctx.append(s->commands, q);
}
bool audio_voice_live(TickContext& ctx, Handle voice) {
    AudioState* s = checked(ctx);
    return s && live(*s, voice);
}
bool audio_fade(TickContext& ctx, Handle voice, f32 target, f32 seconds, FadeCurve curve, bool stop_at_end) {
    AudioState* s = checked(ctx);
    if (!s || !live(*s, voice)) return false;
    AudioRequest q{};
    q.op = ReqOp::Fade; q.voice = voice; q.volume = target; q.seconds = seconds; q.curve = curve; q.stop = stop_at_end;
    return ctx.append(s->commands, q);
}
bool music_play(TickContext& ctx, Handle stream, const MusicPlay& p) {
    AudioState* s = checked(ctx);
    if (!s) return false;
    AudioRequest q{};
    q.op = ReqOp::Music; q.voice = stream; q.music = p;
    return ctx.append(s->commands, q);
}
void music_stop(TickContext& ctx, f32 fade) {
    AudioState* s = checked(ctx);
    if (!s) return;
    AudioRequest q{};
    q.op = ReqOp::Music; q.music.fade_out = fade;
    ctx.append(s->commands, q);
}

Handle audio_play(Engine& e, const SoundPlay& play) { AudioState* s = state(e); return s ? play_in(*s, play) : Handle{}; }
bool   audio_set(Engine& e, Handle voice, f32 volume, f32 pitch, f32 pan) { AudioState* s = state(e); return s && queue_in(*s, MixOp::Set, voice, volume, pitch, pan, 0); }
bool   audio_stop(Engine& e, Handle voice) { AudioState* s = state(e); return s && queue_in(*s, MixOp::Stop, voice, 0, 0, 0, 0); }
void   audio_bus_volume(Engine& e, AudioBus bus, f32 volume) { if (AudioState* s = state(e)) queue_in(*s, MixOp::Bus, Handle{}, volume, 0, 0, (u8)bus); }

bool audio_fade(Engine& e, Handle voice, f32 target, f32 seconds, FadeCurve curve, bool stop_at_end) {
    AudioState* s = state(e);
    if (!s) return false;
    Lock lock(s->cmds.mu);
    return fade_locked(*s, voice, target, seconds, curve, stop_at_end);
}
bool music_play(Engine& e, Handle stream, const MusicPlay& p) {
    AudioState* s = state(e);
    if (!s) return false;
    Lock lock(s->cmds.mu);
    return music_locked(*s, stream, p);
}
void music_stop(Engine& e, f32 fade) {
    AudioState* s = state(e);
    if (!s) return;
    Lock lock(s->cmds.mu);
    MusicPlay p;
    p.fade_out = fade;
    music_locked(*s, Handle{}, p);
}
Handle music_current(Engine& e) {
    AudioState* s = state(e);
    return s ? s->music.stream : Handle{};
}

AudioStats audio_stats(Engine& e) {
    AudioStats st;
    AudioState* s = state(e);
    if (!s) return st;
    Lock lock(s->cmds.mu);
    st.device  = s->device;
    st.dropped = s->cmds.dropped;
    st.steals = s->cmds.steals;
    st.merged = s->cmds.merged;
    st.mixer   = s->mixer.stats;
    for (const VoiceRec& v : s->cmds.voices) {
        if (v.used) ++st.live_voices;
        if (v.used && v.stream) { ++st.stream_voices; st.underruns += v.ring.underruns; }
    }
    return st;
}

} // namespace kv
