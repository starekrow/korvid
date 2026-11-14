// The audio device on Android: an AAudio output stream. Float samples at the rate asked
// (AAudio converts to the device's), the low-latency performance mode, and a data callback
// on AAudio's own realtime thread, which pulls from the mixer as SDL's and RemoteIO's do.
//
// The stream stops while the app is not in the foreground and starts when it is back
// (platform_android_on_foreground: the activity's pause and resume, from the OS's thread),
// and is made again when its device goes away (a headset pulled: AAUDIO_ERROR_DISCONNECTED,
// which the error callback hears; the stream cannot be closed from inside it, so a thread
// does that).
#include "korvid/platform_audio/audio.h"
#include "korvid/platform/android.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/thread.h"
#include "../audio_backend.h"
#include <aaudio/AAudio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

namespace kv {

namespace {

struct AudioDevice {
    AAudioStream*   stream = nullptr;
    AudioDeviceDesc desc;
    AudioDeviceInfo info;
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER; // the stream pointer: open, close, restart, foreground
    bool            foreground = true;
    bool            closing = false;
};

AudioDevice g_audio; // one per process
bool        g_null_device = false;

aaudio_data_callback_result_t audio_data(AAudioStream*, void* user, void* data, int32_t frames) {
    AudioDevice& d = *(AudioDevice*)user;
    if (!thread_is_realtime()) thread_set_realtime(true); // each stream brings a thread of its own
    f32* p = (f32*)data;
    u32 ch = d.info.channels;
    u32 left = (u32)frames;
    while (left) {
        u32 n = left < 1024 ? left : 1024;
        d.desc.callback(d.desc.user, p, n);
        p += (usize)n * ch;
        left -= n;
    }
    if (g_null_device) memset(data, 0, (usize)frames * ch * sizeof(f32));
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void audio_error(AAudioStream*, void*, aaudio_result_t error);

// Opens and starts a stream for g_audio.desc. The lock is held.
bool open_stream() {
    AudioDevice& d = g_audio;
    AAudioStreamBuilder* b = nullptr;
    aaudio_result_t res = AAudio_createStreamBuilder(&b);
    if (res != AAUDIO_OK) { KV_WARN("audio", "AAudio: no stream builder: %s", AAudio_convertResultToText(res)); return false; }
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(b, (int32_t)d.desc.channels);
    AAudioStreamBuilder_setSampleRate(b, (int32_t)d.desc.rate);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_SONIFICATION);
    AAudioStreamBuilder_setDataCallback(b, audio_data, &d);
    AAudioStreamBuilder_setErrorCallback(b, audio_error, &d);
    res = AAudioStreamBuilder_openStream(b, &d.stream);
    AAudioStreamBuilder_delete(b);
    if (res != AAUDIO_OK) { KV_WARN("audio", "AAudio: cannot open a stream: %s", AAudio_convertResultToText(res)); d.stream = nullptr; return false; }
    d.info.rate     = (u32)AAudioStream_getSampleRate(d.stream);
    d.info.channels = (u32)AAudioStream_getChannelCount(d.stream);
    int32_t burst   = AAudioStream_getFramesPerBurst(d.stream);
    d.info.frames   = burst > 0 ? (u32)burst : d.desc.frames;
    // Two bursts of buffer: the least that rides out one late callback.
    if (burst > 0) AAudioStream_setBufferSizeInFrames(d.stream, burst * 2);
    if (d.info.channels != d.desc.channels || AAudioStream_getFormat(d.stream) != AAUDIO_FORMAT_PCM_FLOAT) {
        KV_WARN("audio", "AAudio gave %u channels in format %d, not %u in float", d.info.channels, (int)AAudioStream_getFormat(d.stream), d.desc.channels);
        AAudioStream_close(d.stream);
        d.stream = nullptr;
        return false;
    }
    d.info.open = true; // before the first callback can run
    if (d.foreground) {
        res = AAudioStream_requestStart(d.stream);
        if (res != AAUDIO_OK) KV_WARN("audio", "AAudio: the stream did not start: %s", AAudio_convertResultToText(res));
    }
    return true;
}

void* restart_thread(void*) {
    AudioDevice& d = g_audio;
    pthread_mutex_lock(&d.lock);
    if (d.stream && !d.closing) {
        AAudioStream_close(d.stream);
        d.stream = nullptr;
        if (open_stream()) KV_INFO("audio", "device changed: a new stream, %u Hz, %u frames per burst", d.info.rate, d.info.frames);
        else KV_WARN("audio", "device changed and no new stream opened: silent");
    }
    pthread_mutex_unlock(&d.lock);
    return nullptr;
}

void audio_error(AAudioStream*, void*, aaudio_result_t error) {
    if (error != AAUDIO_ERROR_DISCONNECTED) return; // anything else: the stream is as dead as it is
    pthread_t t;
    if (pthread_create(&t, nullptr, restart_thread, nullptr) == 0) pthread_detach(t);
}

// The activity's pause and resume, on the OS's thread.
void on_foreground(bool foreground) {
    AudioDevice& d = g_audio;
    pthread_mutex_lock(&d.lock);
    d.foreground = foreground;
    if (d.stream) {
        if (foreground) AAudioStream_requestStart(d.stream);
        else            AAudioStream_requestStop(d.stream);
    }
    pthread_mutex_unlock(&d.lock);
}

} // namespace

void platform_audio_null_device(bool on) { g_null_device = on; }

bool platform_audio_open(Engine& e, const AudioDeviceDesc& desc) {
    KV_UNUSED(e);
    AudioDevice& d = g_audio;
    if (d.stream) { KV_ERROR("audio", "a device is already open"); return false; }
    if (!desc.callback || desc.channels == 0 || desc.channels > 8 || desc.rate == 0) { KV_ERROR("audio", "bad device description"); return false; }
    if (const char* driver = getenv("SDL_AUDIO_DRIVER"); driver && strcmp(driver, "dummy") == 0) g_null_device = true;
    pthread_mutex_lock(&d.lock);
    d.desc    = desc;
    d.info    = AudioDeviceInfo{};
    d.closing = false;
    bool ok = open_stream();
    pthread_mutex_unlock(&d.lock);
    if (!ok) { d.info = AudioDeviceInfo{}; return false; }
    platform_android_on_foreground(on_foreground);
    KV_INFO("audio", "device open: %u Hz, %u channels, %u frames per burst, driver 'aaudio'%s", d.info.rate, d.info.channels, d.info.frames,
            g_null_device ? ", silent" : "");
    return true;
}

void platform_audio_close(Engine& e) {
    KV_UNUSED(e);
    AudioDevice& d = g_audio;
    platform_android_on_foreground(nullptr);
    pthread_mutex_lock(&d.lock);
    d.closing = true;
    AAudioStream* s = d.stream;
    d.stream = nullptr;
    pthread_mutex_unlock(&d.lock);
    if (!s) return;
    AAudioStream_requestStop(s);
    AAudioStream_close(s); // returns once the callbacks have stopped
    d.info = AudioDeviceInfo{};
    KV_INFO("audio", "device closed");
}

const AudioDeviceInfo& platform_audio(Engine& e) {
    KV_UNUSED(e);
    return g_audio.info;
}

bool audio_backend_startup() { return true; }
void audio_backend_shutdown() {}

} // namespace kv
