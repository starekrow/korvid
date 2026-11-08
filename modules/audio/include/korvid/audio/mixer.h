// Korvid audio: the mixer, as a function over a buffer. It owns the voices and the two
// rings between the game thread and the device thread: commands in, finished voices
// out. mixer_render runs on the device thread (the platform's callback) and must not
// allocate, lock or log; the tests call it directly with a buffer of their own. No SDL
// anywhere in here.
#pragma once
#include "korvid/core/types.h"

namespace kv {

constexpr u32 KV_AUDIO_VOICES   = 64;
constexpr u32 KV_AUDIO_CMD_RING = 1024;

enum class AudioBus : u8 { Master, Sfx, Music, Ui, Count };

// A single-producer, single-consumer ring: the game thread pushes, the device thread
// pops (or the other way round for the finished ring). Acquire/release on the indices
// is the whole protocol.
template <class T, u32 N>
struct SpscRing {
    T   items[N];
    u32 head = 0; // next to pop; the consumer's
    u32 tail = 0; // next to push; the producer's

    bool push(const T& v) {
        u32 t = __atomic_load_n(&tail, __ATOMIC_RELAXED);
        u32 next = (t + 1) % N;
        if (next == __atomic_load_n(&head, __ATOMIC_ACQUIRE)) return false; // full
        items[t] = v;
        __atomic_store_n(&tail, next, __ATOMIC_RELEASE);
        return true;
    }
    bool pop(T& out) {
        u32 h = __atomic_load_n(&head, __ATOMIC_RELAXED);
        if (h == __atomic_load_n(&tail, __ATOMIC_ACQUIRE)) return false; // empty
        out = items[h];
        __atomic_store_n(&head, (h + 1) % N, __ATOMIC_RELEASE);
        return true;
    }
};

// Retire: stop every voice reading `samples`, then acknowledge with KV_MIX_RETIRED | ticket
// on the done ring, after which nothing on the device thread reads them.
enum class MixOp : u8 { Play, Set, Stop, PauseSim, ResumeSim, Bus, Retire };

constexpr u64 KV_MIX_RETIRED = 0xFFFFFFFFull; // a done entry's slot half meaning "retired", the ticket in gen

// What crosses to the device thread. A voice is a slot and a generation, so a command
// for a voice that already finished and whose slot was reused is ignored.
struct MixCmd {
    MixOp      op;
    u8         bus;
    u32        slot;
    u32        gen;
    const f32* samples;  // Play: the sound's frames; Retire: the frames to let go of (gen is the ticket)
    u32        frames, rate, channels;
    f32        volume, pitch, pan;
    bool       loop, pause_with_sim;
};

struct MixVoice {
    const f32* samples = nullptr;
    u32        frames = 0, rate = 0, channels = 0;
    f64        pos = 0.0; // in source frames
    f32        volume = 1.0f, pitch = 1.0f, pan = 0.0f;
    u8         bus = 0;
    bool       loop = false, pause_with_sim = true, active = false;
    u32        gen = 0;
};

struct MixerStats {
    u64 frames_rendered = 0; // output frames
    u64 callbacks       = 0;
    u32 active          = 0; // voices playing at the last render
    u32 peak_active     = 0;
    u32 clipped         = 0; // samples past [-1, 1] at the output
};

struct Mixer {
    MixVoice   voices[KV_AUDIO_VOICES];
    f32        bus_gain[(u32)AudioBus::Count] = {1.0f, 1.0f, 1.0f, 1.0f};
    bool       sim_paused = false;
    u32        out_rate = 48000, out_channels = 2;
    MixerStats stats;        // written by the device thread; read as a snapshot
    SpscRing<MixCmd, KV_AUDIO_CMD_RING> in;   // game -> device
    SpscRing<u64, KV_AUDIO_VOICES * 4>  done; // device -> game: slot | gen << 32, or KV_MIX_RETIRED | ticket << 32
    u64        acks[16];     // retire acknowledgements the done ring had no room for, retried each render
    u32        ack_count = 0;
};

void mixer_init(Mixer& m, u32 out_rate, u32 out_channels);
// The device thread: apply every pending command, then mix `frames` interleaved frames
// into `out` (overwritten, not accumulated). Voices that end report to `done`.
void mixer_render(Mixer& m, f32* out, u32 frames);

} // namespace kv
