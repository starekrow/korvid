#include "korvid/platform/audio.h"
#include "korvid/platform/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/thread.h"
#include <SDL3/SDL.h>

namespace kv {

namespace {

struct AudioDevice {
    SDL_AudioStream* stream = nullptr;
    AudioDeviceDesc  desc;
    AudioDeviceInfo  info;
    bool             marked = false; // the callback's thread has been marked realtime
};

AudioDevice g_audio; // one per process, like the SDL audio subsystem itself

// SDL's thread: `additional` bytes are wanted now. Mixed in slices of at most 1024
// frames from a stack buffer; no allocation, no logging.
void SDLCALL audio_pull(void* user, SDL_AudioStream* stream, int additional, int) {
    AudioDevice& d = *(AudioDevice*)user;
    if (!d.marked) { thread_set_realtime(true); d.marked = true; }
    u32 bytes_per_frame = d.info.channels * 4;
    u32 frames = bytes_per_frame ? (u32)additional / bytes_per_frame : 0;
    f32 buf[1024 * 8];
    while (frames) {
        u32 n = frames < 1024 ? frames : 1024;
        d.desc.callback(d.desc.user, buf, n);
        SDL_PutAudioStreamData(stream, buf, (int)(n * bytes_per_frame));
        frames -= n;
    }
}

} // namespace

bool platform_audio_open(Engine& e, const AudioDeviceDesc& desc) {
    KV_UNUSED(e);
    if (g_audio.stream) { KV_ERROR("audio", "a device is already open"); return false; }
    if (!desc.callback || desc.channels == 0 || desc.channels > 8 || desc.rate == 0) { KV_ERROR("audio", "bad device description"); return false; }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) { KV_WARN("audio", "SDL audio unavailable: %s", SDL_GetError()); return false; }
    char frames_hint[16];
    SDL_snprintf(frames_hint, sizeof frames_hint, "%u", desc.frames);
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, frames_hint);
    g_audio.desc = desc;
    g_audio.info.rate     = desc.rate;
    g_audio.info.channels = desc.channels;
    g_audio.info.frames   = desc.frames;
    g_audio.marked = false;
    SDL_AudioSpec spec{SDL_AUDIO_F32, (int)desc.channels, (int)desc.rate};
    g_audio.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_pull, &g_audio);
    if (!g_audio.stream) {
        KV_WARN("audio", "no playback device: %s", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    SDL_AudioSpec dev{};
    int dev_frames = 0;
    if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(g_audio.stream), &dev, &dev_frames) && dev_frames > 0)
        g_audio.info.frames = (u32)dev_frames;
    g_audio.info.open = true;
    SDL_ResumeAudioStreamDevice(g_audio.stream);
    KV_INFO("audio", "device open: %u Hz, %u channels, %u frames per buffer (hardware %d Hz, %d channels), driver '%s'",
            g_audio.info.rate, g_audio.info.channels, g_audio.info.frames, dev.freq, dev.channels, SDL_GetCurrentAudioDriver());
    return true;
}

void platform_audio_close(Engine& e) {
    KV_UNUSED(e);
    if (!g_audio.stream) return;
    SDL_DestroyAudioStream(g_audio.stream); // stops the callbacks before returning
    g_audio.stream = nullptr;
    g_audio.info = AudioDeviceInfo{};
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    KV_INFO("audio", "device closed");
}

const AudioDeviceInfo& platform_audio(Engine& e) {
    KV_UNUSED(e);
    return g_audio.info;
}

} // namespace kv
