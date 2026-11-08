# platform_audio

The audio device: one playback stream whose callback runs on its own realtime thread and
fills interleaved f32 frames. SDL3 is the backend. The audio module mixes into it; a tool
that makes noise can link this without a window, and a tool that is silent leaves it out.

## Using it

`deps = { "core", "platform", "platform_audio" }`; the audio module depends on it
already. `platform_audio_open(e, desc)` opens the default device (false, with the reason
logged, when there is none: the game runs silent), `platform_audio_close(e)` closes it,
and the module's shutdown closes it whoever opened it. The callback must not allocate,
lock, log or block; `thread_is_realtime()` is true inside it.

## Quick reference

Header `korvid/platform_audio/audio.h`:

| name | kind | what |
|---|---|---|
| `AudioDeviceDesc` | record | `rate` (48000), `channels` (2), `frames` (512), `callback(user, out, frames)`, `user` |
| `AudioDeviceInfo` | record | `rate`, `channels`, `frames`, `open` |
| `platform_audio_open(e, desc)`, `platform_audio_close(e)`, `platform_audio(e)` | functions | the default playback device as a stream; the callback runs on SDL's thread, marked realtime, and fills interleaved f32 |

Header `korvid/platform_audio/module.h`: `platform_audio_module`, the record.

## Behaviour worth knowing

- **SDL audio comes up on open and goes on close** (`SDL_InitSubSystem(SDL_INIT_AUDIO)`),
  so linking the module costs nothing until something plays. `SDL_AUDIO_DRIVER=dummy`
  gives a test a device with no sound card.
- **SDL shuts down with the last user**, as in `platform_hid`.

## Not in this module

More than one device, capture, device hot-switching, Windows and macOS backends.
