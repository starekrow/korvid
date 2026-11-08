// Korvid audio: playing sounds, from the game's side. A voice plays one sound asset with
// a volume, a pitch (the playback ratio: 2 is an octave up and half the length), a pan
// in [-1, 1], looping or not, on a bus. audio_play returns a voice handle at once from a
// pool on the game's side; the start crosses to the mixer at the end of the tick
// ("audio.extract", phase PostSim), or as soon as the sound asset is Ready if it was not
// yet. audio_set and audio_stop follow the same path. VoiceFinished arrives when the
// voice ran out or was stopped; the handle is stale from then on.
//
// A voice started with pause_with_sim (the default) holds while the sim is paused and
// goes on at the resume; a menu click or music says false and plays through. The
// Master bus scales everything; Sfx, Music and Ui scale their own.
//
// Call these from a system or handler that declares a write on "audio.commands". They
// take a lock, briefly, so any thread of the schedule may call them; the voice pool and
// the command list are the game thread's, never the mixer's.
#pragma once
#include "korvid/audio/mixer.h"
#include "korvid/core/handle.h"

namespace kv {

struct Engine;
struct TickContext;

struct SoundPlay {
    Handle   sound;                 // asset.sound or asset.stream; a Pending one starts when it is Ready, a Failed one finishes at once
    f32      volume = 1.0f;
    f32      pitch  = 1.0f;
    f32      pan    = 0.0f;         // -1 left, 0 center, 1 right
    bool     loop   = false;
    bool     pause_with_sim = true;
    AudioBus bus    = AudioBus::Sfx;
};

struct VoiceFinished { Handle voice; }; // no schema on purpose: the handle is already stale when this arrives

Handle audio_play(TickContext& ctx, const SoundPlay& play);
bool   audio_set(TickContext& ctx, Handle voice, f32 volume, f32 pitch, f32 pan);
bool   audio_stop(TickContext& ctx, Handle voice);
void   audio_bus_volume(TickContext& ctx, AudioBus bus, f32 volume);
bool   audio_voice_live(TickContext& ctx, Handle voice); // started or waiting, not yet finished

// Ramps a voice's volume to `target` over `seconds` in the mixer, frame by frame, linear or
// equal-power, from wherever it is (a fade replaces a fade); with stop_at_end the voice
// finishes when it gets there. A fade asked for before the voice has started is sent with
// the start.
bool   audio_fade(TickContext& ctx, Handle voice, f32 target, f32 seconds, FadeCurve curve = FadeCurve::EqualPower, bool stop_at_end = false);

// Music: one track at a time on the Music bus, looping (at its loop points), playing
// through pauses. music_play with the track already playing does nothing; otherwise the
// current track fades out over `fade_out` (equal power) and the new one starts `overlap`
// seconds before that fade ends (0: after it; fade_out: together, a true crossfade) and
// fades in over `fade_in` to `volume`. A request during a crossfade fades out what is
// fading in and takes its place. music_stop fades the music out; music_current is the
// stream playing or about to, or null.
struct MusicPlay {
    f32 fade_out = 1.0f;
    f32 fade_in  = 1.0f;
    f32 overlap  = 1.0f;
    f32 volume   = 1.0f;
};
bool   music_play(TickContext& ctx, Handle stream, const MusicPlay& p = MusicPlay{});
void   music_stop(TickContext& ctx, f32 fade);

// The same, from outside the schedule (startup, a tool).
Handle audio_play(Engine& e, const SoundPlay& play);
bool   audio_set(Engine& e, Handle voice, f32 volume, f32 pitch, f32 pan);
bool   audio_stop(Engine& e, Handle voice);
void   audio_bus_volume(Engine& e, AudioBus bus, f32 volume);
bool   audio_fade(Engine& e, Handle voice, f32 target, f32 seconds, FadeCurve curve = FadeCurve::EqualPower, bool stop_at_end = false);
bool   music_play(Engine& e, Handle stream, const MusicPlay& p = MusicPlay{});
void   music_stop(Engine& e, f32 fade);
Handle music_current(Engine& e);

struct AudioStats {
    bool       device = false;   // a device is open; otherwise every voice finishes at once
    u32        live_voices = 0;  // the game's view: started or waiting
    u32        dropped = 0;      // commands that found the ring full (lost)
    u32        stream_voices = 0;
    u32        underruns = 0;    // buffers a stream voice found its ring empty, over the live stream voices
    MixerStats mixer;            // a snapshot from the device thread
};
AudioStats audio_stats(Engine& e);

} // namespace kv
