// Korvid platform_audio: the audio device. One per process, opened on request by whoever
// mixes (the audio module) and closed at module shutdown. The device pulls: SDL calls
// the callback on its own thread whenever it wants more frames, and the callback fills
// interleaved f32 frames at the device's rate and channel count. That thread is a
// realtime thread: the callback must not allocate, lock, log or block; it reads what the
// game thread hands it through lock-free rings and writes samples. thread_is_realtime()
// is true inside it. If no device can be opened the call fails and the game runs silent.
#pragma once
#include "korvid/core/types.h"

namespace kv {

struct Engine;

typedef void (*AudioCallback)(void* user, f32* out, u32 frames);

struct AudioDeviceDesc {
    u32           rate     = 48000; // asked for; the device may differ, see AudioDeviceInfo
    u32           channels = 2;
    u32           frames   = 512;   // the buffer size hint, frames per callback
    AudioCallback callback = nullptr;
    void*         user     = nullptr;
};

struct AudioDeviceInfo {
    u32  rate     = 0;    // what the stream delivers to the callback
    u32  channels = 0;
    u32  frames   = 0;    // the device's buffer, frames; a callback asks for about this many
    bool open     = false;
};

// Opens the default playback device as a stream in the desc's format (SDL converts to
// the hardware's). False, with the reason logged, if there is none or one is open already.
bool                   platform_audio_open(Engine& e, const AudioDeviceDesc& desc);
void                   platform_audio_close(Engine& e);
// The open device's format, or a record with open == false.
const AudioDeviceInfo& platform_audio(Engine& e);
// Before the device opens: true makes it a null device that plays nothing (SDL's dummy
// driver), for tests and headless runs. SDL_AUDIO_DRIVER in the environment still wins; on
// iOS, which has no SDL, SDL_AUDIO_DRIVER=dummy makes the device a null one too.
void                   platform_audio_null_device(bool on);

} // namespace kv
