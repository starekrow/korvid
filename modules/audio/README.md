# audio

Sound, in the shape the requirements asked for: a realtime mixer on the device's thread,
fed through lock-free rings from the game thread, with voices that carry a volume, a
pitch, a pan and a loop over sound assets, four buses, and voices that pause with the
sim. The game thread never blocks on audio; the device thread never allocates, locks or
logs. A machine without a device runs the game silent.

## How it works

**Sounds are assets** (`korvid/asset/sound.h`, in the asset module). `asset_load_sound(e,
"mygame/shot.wav")` returns a handle at once; `asset.pump_sounds` decodes WAV (PCM 8, 16,
24 and 32-bit and 32-bit float, our own reader) or Ogg Vorbis (stb_vorbis, vendored, with
a work buffer from scratch so it never calls malloc) into interleaved f32 at the file's
own rate. The samples live until shutdown, so the mixer reads them without a lock.

**Voices** (`audio.h`). `audio_play(ctx, SoundPlay{...})` takes a slot in a 64-voice table
on the game's side and returns its handle (kind `audio.voice`) at once. `audio_set`,
`audio_stop` and `audio_bus_volume` queue commands. All of them take a lock briefly, so
a system on any thread and a handler alike may call them; they check that the caller
declared a write on `audio.commands`.

**The extract.** `audio.extract` (PostSim, main thread, every tick, paused or not):

1. pops the finished ring: each slot the mixer is done with becomes a `VoiceFinished`
   event and a free slot (the handle is stale from then on; the event has no schema on
   purpose, so the bus does not drop it as stale);
2. tells the mixer when the sim pauses or resumes;
3. starts the voices that are waiting: a voice whose sound is `Ready` crosses as a `Play`
   with the sample pointer, length, rate and channels; a `Failed` sound, a stop before
   the start, or no device at all finishes the voice at once;
4. pushes the tick's `Set`, `Stop` and bus commands in order.

**The mixer** (`mixer.h`, `mixer.cc`) is a function over a buffer, with no SDL in it:
`mixer_render(m, out, frames)` drains the command ring, then for each active voice reads
samples at `rate_in / rate_out * pitch` with linear interpolation (mono to both channels,
stereo as is, more channels summed), applies equal-power pan, the voice's volume, its bus
and the master, sums into the output, clamps and counts clipping, and reports voices
that ran out. A voice with `pause_with_sim` is held where it is while the sim is paused.
Commands name a slot and a generation, so a command for a voice that already finished
and whose slot was reused is ignored. The platform's device callback (`platform/audio.h`,
SDL's thread, marked realtime) calls it in slices of at most 1024 frames.

The rings are single-producer, single-consumer with acquire/release on the indices; the
tests push from a second thread to check them.

## Using it

```cpp
// Startup.
g->shot = asset_load_sound(engine, "mygame/shot.wav");

// A system or handler that declares a write on "audio.commands".
SoundPlay p;
p.sound  = g.shot;
p.volume = 0.5f;
p.pitch  = g.rng.range(0.9f, 1.1f);   // a little variety per shot
p.pan    = at.x / 900.0f;
audio_play(ctx, p);

// A loop whose volume follows the game, stopped when its owner dies.
SoundPlay loop; loop.sound = g.thrust; loop.loop = true; loop.volume = 0.0f;
g.thrust_voice = audio_play(ctx, loop);
audio_set(ctx, g.thrust_voice, thrusting ? 0.35f : 0.0f, 1.0f, 0.0f);
audio_stop(ctx, g.thrust_voice);

// A menu click that plays while the sim is paused.
SoundPlay click; click.sound = g.click; click.bus = AudioBus::Ui; click.pause_with_sim = false;
```

Add `"audio"` to the module's `deps`; it brings `platform` and `asset`.

## Quick reference

Header `korvid/audio/audio.h`:

| name | kind | what |
|---|---|---|
| `SoundPlay` | record | `sound`, `volume` (1), `pitch` (1), `pan` (0), `loop` (false), `pause_with_sim` (true), `bus` (`Sfx`) |
| `AudioBus` | enum | `Master`, `Sfx`, `Music`, `Ui` |
| `audio_play(ctx, play)` | function | a voice handle at once; null when all 64 are in use |
| `audio_set(ctx, voice, volume, pitch, pan)`, `audio_stop(ctx, voice)` | functions | false for a stale voice |
| `audio_bus_volume(ctx, bus, volume)` | function | a bus gain; `Master` scales everything |
| `audio_voice_live(ctx, voice)` | function | started or waiting, not finished |
| `VoiceFinished{voice}` | event | the voice ran out, was stopped, or could not play |
| `audio_stats(e)` | function | `device`, `live_voices`, `dropped`, and the mixer's counters |

The same calls take an `Engine&` from outside the schedule.

Header `korvid/audio/mixer.h`: `Mixer`, `MixCmd`, `MixOp`, `SpscRing<T, N>`, `mixer_init`,
`mixer_render`, `KV_AUDIO_VOICES` (64), `KV_AUDIO_CMD_RING` (1024).

Header `korvid/asset/sound.h` (asset module): `SoundAsset`, `asset_load_sound`, `asset_sound`,
`sound_decode`, `sound_decode_wav`, `sound_decode_ogg`.

Header `korvid/platform/audio.h` (platform module): `AudioDeviceDesc`, `AudioDeviceInfo`,
`platform_audio_open`, `platform_audio_close`, `platform_audio`.

What the module registers with the engine:

| name | kind | notes |
|---|---|---|
| `audio.voice` | handle kind | `kv_handle_describe` prints playing or waiting, volume, pitch |
| `VoiceFinished` | event type | no schema |
| `audio.commands` | resource, published | declare a write on it to play, set and stop |
| `audio.extract` | system, phase PostSim, main thread, `Always` | writes `audio.commands` |

Depends on: core, math, platform, asset.

## Behaviour worth knowing

- **Starts land at buffer boundaries.** A voice played this tick starts at the beginning
  of the next device buffer the mixer renders after the extract: at 512 frames and 48 kHz
  that is up to about 11 ms plus a tick. Placement within the buffer by tick time is the
  next step if a game can hear the difference.
- **A ring full drops the command** and logs the count once per change; 1024 commands a
  tick is the budget. A slot table full returns a null voice.
- **Pitch is a ratio**, not semitones: 2 is an octave up and half the length. The mixer
  interpolates linearly; a large pitch on a bright sound aliases.
- **Loops join sample to sample.** Make the file's end meet its start (the rocks
  thruster cross-fades its tail into its head when generated).
- **The device is 48 kHz stereo** from the module's point of view; SDL converts to the
  hardware's format. Sounds at other rates are read at the ratio.
- **Without a device** every voice finishes on the tick after it starts, so logic that
  waits for `VoiceFinished` still runs.
- **A sound can retire while it plays.** When the asset module retires a sound (its last
  scope let go), the handle goes stale at once and the module hands the samples here:
  `audio.extract` sends the mixer a `Retire`, the mixer stops every voice on those
  samples (each reports `VoiceFinished`) and answers on the done ring, and only then are
  the samples freed. A voice still waiting for a sound that retired finishes without
  playing. Without a device the samples go at once.

## Not in this module

Streaming from disk, effects and filters, 3D or distance attenuation, more than one
device, capture, music cross-fades, sample-accurate starts (the tick stamp is not yet
used by the mixer), and a mixer graph.
