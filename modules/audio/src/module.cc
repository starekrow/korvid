#include "korvid/audio/module.h"
#include "korvid/asset/sound.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/profile.h"
#include "korvid/platform/audio.h"
#include <pthread.h>
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
};

// A command the game queued this tick, applied in order by the extract.
struct Queued {
    MixOp  op;
    Handle voice;
    f32    volume, pitch, pan;
    u8     bus;
};

struct AudioCommands {
    VoiceRec        voices[KV_AUDIO_VOICES];
    Array<Queued>   queued;
    pthread_mutex_t mu;
    u32             dropped = 0;
};

struct AudioState {
    Mixer         mixer;
    AudioCommands cmds;
    u16           kind = 0;
    bool          device = false;
    bool          last_paused = false;
    u32           dropped_logged = 0;
};

AudioState* state(Engine& e) { return engine_module_state_as<AudioState>(e, "audio"); }

struct Lock {
    pthread_mutex_t& mu;
    explicit Lock(pthread_mutex_t& m) : mu(m) { pthread_mutex_lock(&mu); }
    ~Lock() { pthread_mutex_unlock(&mu); }
};

void* voice_resolve(kv_handle h, void* ctx) {
    AudioCommands& c = *(AudioCommands*)ctx;
    Handle hh{h};
    u32 i = hh.index();
    if (i >= KV_AUDIO_VOICES || !c.voices[i].used || c.voices[i].gen != hh.gen()) return nullptr;
    return &c.voices[i];
}
int voice_describe(kv_handle h, void* ctx, char* out, size_t cap) {
    VoiceRec* v = (VoiceRec*)voice_resolve(h, ctx);
    if (!v) return snprintf(out, cap, "stale");
    return snprintf(out, cap, "%s vol %.2f pitch %.2f%s", v->started ? "playing" : "waiting", v->play.volume, v->play.pitch, v->play.loop ? " loop" : "");
}

// The device thread.
void device_callback(void* user, f32* out, u32 frames) { mixer_render(*(Mixer*)user, out, frames); }

Handle play_in(AudioState& s, const SoundPlay& play) {
    Lock lock(s.cmds.mu);
    for (u32 i = 0; i < KV_AUDIO_VOICES; ++i) {
        VoiceRec& v = s.cmds.voices[i];
        if (v.used) continue;
        v.used = true;
        v.started = v.stopped = false;
        v.play = play;
        if (v.gen == 0 || v.gen == 0xFFFFF) v.gen = 1; else ++v.gen;
        return Handle::make(s.kind, v.gen, i);
    }
    ++s.cmds.dropped;
    return Handle{};
}

bool queue_in(AudioState& s, MixOp op, Handle voice, f32 volume, f32 pitch, f32 pan, u8 bus) {
    Lock lock(s.cmds.mu);
    if (op != MixOp::Bus && !voice_resolve(voice.bits, &s.cmds)) return false;
    s.cmds.queued.push(Queued{op, voice, volume, pitch, pan, bus});
    return true;
}

void free_voice(AudioState& s, u32 slot) {
    VoiceRec& v = s.cmds.voices[slot];
    v.used = false;
    v.started = false;
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
        VoiceRec& v = s.cmds.voices[slot];
        if (!v.used || v.gen != gen) continue;
        ctx.emit(VoiceFinished{Handle::make(s.kind, gen, slot)});
        free_voice(s, slot);
    }

    if (ctx.paused != s.last_paused) {
        s.last_paused = ctx.paused;
        MixCmd c{};
        c.op = ctx.paused ? MixOp::PauseSim : MixOp::ResumeSim;
        if (!s.mixer.in.push(c)) ++s.cmds.dropped;
    }

    for (u32 i = 0; i < KV_AUDIO_VOICES; ++i) {
        VoiceRec& v = s.cmds.voices[i];
        if (!v.used || v.started) continue;
        const SoundAsset* snd = asset_sound(e, v.play.sound);
        bool finish = v.stopped || !snd || snd->state == AssetState::Failed || !s.device;
        if (!finish && snd->state != AssetState::Ready) continue; // waits for the file
        if (finish) {
            ctx.emit(VoiceFinished{Handle::make(s.kind, v.gen, i)});
            free_voice(s, i);
            continue;
        }
        if (s.mixer.in.push(cmd_for(v, i, snd))) v.started = true;
        else ++s.cmds.dropped;
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
        } else if (!v->started) { v->stopped = true; continue; } // finishes next tick, never plays
        MixCmd c{};
        c.op = q.op; c.slot = slot; c.gen = v->gen;
        c.volume = q.volume; c.pitch = q.pitch; c.pan = q.pan;
        if (!s.mixer.in.push(c)) ++s.cmds.dropped;
    }
    s.cmds.queued.clear();

    if (s.cmds.dropped != s.dropped_logged) {
        KV_WARN("audio", "%u commands dropped (ring full or no free voice)", s.cmds.dropped - s.dropped_logged);
        s.dropped_logged = s.cmds.dropped;
    }
}

bool audio_startup(Engine& e, ModuleInstance& self) {
    AudioState* s = arena_push<AudioState>(self.arena);
    if (!s) return false;
    pthread_mutex_init(&s->cmds.mu, nullptr);
    s->cmds.queued.alloc = &self.general;
    kv_handle_kind_desc desc{};
    desc.name = "audio.voice"; desc.type_key = type_key<VoiceRec>(); desc.ctx = &s->cmds;
    desc.resolve = voice_resolve; desc.describe = voice_describe; desc.flags = KV_HANDLE_GENERATIONAL;
    s->kind = handle_kind_register(desc);
    if (!s->kind) return false;
    if (!engine_publish(e, self, ".commands", &s->cmds)) return false;
    self.set_state(s);

    AudioDeviceDesc dd;
    dd.rate = 48000; dd.channels = 2; dd.frames = 512;
    dd.callback = device_callback; dd.user = &s->mixer;
    mixer_init(s->mixer, dd.rate, dd.channels);
    s->device = platform_audio_open(e, dd);
    if (!s->device) KV_WARN("audio", "no device: running silent, voices finish at once");
    return true;
}

void audio_shutdown(Engine& e, ModuleInstance& self) {
    AudioState* s = self.state_as<AudioState>();
    if (!s) return;
    platform_audio_close(e); // the callbacks have stopped when this returns
    KV_INFO("audio", "%llu frames rendered in %llu callbacks, peak %u voices, %u clipped samples, %u commands dropped",
            (unsigned long long)s->mixer.stats.frames_rendered, (unsigned long long)s->mixer.stats.callbacks,
            s->mixer.stats.peak_active, s->mixer.stats.clipped, s->cmds.dropped);
    kv_handle_kind_unregister(s->kind);
    s->cmds.queued.free();
    pthread_mutex_destroy(&s->cmds.mu);
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
        snprintf(name, sizeof name, "%u", i);
        if (const SoundAsset* snd = asset_sound(*ctx.engine, v.play.sound)) snprintf(sound, sizeof sound, "%s", snd->path);
        snprintf(summary, sizeof summary, "%s %s, vol %.2f, pitch %.2f, pan %.2f%s%s", v.started ? "playing" : "waiting", sound, v.play.volume, v.play.pitch, v.play.pan,
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
        snprintf(summary, sizeof summary, "%.2f", s->mixer.bus_gain[b]); // a snapshot: the device thread owns it
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
constinit const ResourceDesc  k_resources[] = {{".commands", true}};
constinit const SystemDesc    k_systems[] = {
    {.name = ".extract", .phase = Phase::PostSim, .fn = audio_extract, .writes = ".commands",
     .main_thread = true, .runs = RunWhen::Always},
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
AudioState* checked(TickContext& ctx) {
    static const u32 k_cmds = ctx.resource_id("audio.commands"); // full: callers are other modules' systems
    if (!ctx.resource<AudioCommands>(k_cmds)) return nullptr; // undeclared: the context has said so
    return state(*ctx.engine);
}
} // namespace

Handle audio_play(TickContext& ctx, const SoundPlay& play) { AudioState* s = checked(ctx); return s ? play_in(*s, play) : Handle{}; }
bool   audio_set(TickContext& ctx, Handle voice, f32 volume, f32 pitch, f32 pan) { AudioState* s = checked(ctx); return s && queue_in(*s, MixOp::Set, voice, volume, pitch, pan, 0); }
bool   audio_stop(TickContext& ctx, Handle voice) { AudioState* s = checked(ctx); return s && queue_in(*s, MixOp::Stop, voice, 0, 0, 0, 0); }
void   audio_bus_volume(TickContext& ctx, AudioBus bus, f32 volume) { if (AudioState* s = checked(ctx)) queue_in(*s, MixOp::Bus, Handle{}, volume, 0, 0, (u8)bus); }
bool   audio_voice_live(TickContext& ctx, Handle voice) {
    AudioState* s = checked(ctx);
    if (!s) return false;
    Lock lock(s->cmds.mu);
    return voice_resolve(voice.bits, &s->cmds) != nullptr;
}

Handle audio_play(Engine& e, const SoundPlay& play) { AudioState* s = state(e); return s ? play_in(*s, play) : Handle{}; }
bool   audio_set(Engine& e, Handle voice, f32 volume, f32 pitch, f32 pan) { AudioState* s = state(e); return s && queue_in(*s, MixOp::Set, voice, volume, pitch, pan, 0); }
bool   audio_stop(Engine& e, Handle voice) { AudioState* s = state(e); return s && queue_in(*s, MixOp::Stop, voice, 0, 0, 0, 0); }
void   audio_bus_volume(Engine& e, AudioBus bus, f32 volume) { if (AudioState* s = state(e)) queue_in(*s, MixOp::Bus, Handle{}, volume, 0, 0, (u8)bus); }

AudioStats audio_stats(Engine& e) {
    AudioStats st;
    AudioState* s = state(e);
    if (!s) return st;
    Lock lock(s->cmds.mu);
    st.device  = s->device;
    st.dropped = s->cmds.dropped;
    st.mixer   = s->mixer.stats;
    for (const VoiceRec& v : s->cmds.voices) if (v.used) ++st.live_voices;
    return st;
}

} // namespace kv
