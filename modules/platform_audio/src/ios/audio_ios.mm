// The audio device on iOS: the RemoteIO audio unit, its render callback pulling interleaved
// f32 frames from the engine's callback on Core Audio's realtime thread. The session is
// Ambient: the game's sound mixes with other apps' and is silenced by the ring switch, as a
// game's should be until it asks otherwise (slice 8: the audio session's policy). The
// "null device" (tests) is the same unit with its output zeroed after the callback runs, so
// the callback keeps a device's pace.
#include "korvid/platform_audio/audio.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/thread.h"
#include "../audio_backend.h"
#import <AVFAudio/AVFAudio.h>
#include <AudioToolbox/AudioToolbox.h>
#include <string.h>

namespace kv {

namespace {

struct AudioDevice {
    AudioComponentInstance unit = nullptr;
    AudioDeviceDesc        desc;
    AudioDeviceInfo        info;
    bool                   marked = false; // the render thread has been marked realtime
};

AudioDevice g_audio; // one per process
bool        g_null_device = false;

// Core Audio's thread: `frames` interleaved frames into the one buffer. Mixed in slices of
// at most 1024 frames; no allocation, no logging.
OSStatus audio_render(void* user, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 frames, AudioBufferList* io) {
    AudioDevice& d = *(AudioDevice*)user;
    if (!d.marked) { thread_set_realtime(true); d.marked = true; }
    if (!io || io->mNumberBuffers < 1 || !io->mBuffers[0].mData) return noErr;
    f32* out = (f32*)io->mBuffers[0].mData;
    u32 ch = d.info.channels;
    u32 left = frames;
    if ((usize)frames * ch * sizeof(f32) > io->mBuffers[0].mDataByteSize) left = io->mBuffers[0].mDataByteSize / (ch * (u32)sizeof(f32));
    f32* p = out;
    while (left) {
        u32 n = left < 1024 ? left : 1024;
        d.desc.callback(d.desc.user, p, n);
        p += (usize)n * ch;
        left -= n;
    }
    if (g_null_device) memset(out, 0, io->mBuffers[0].mDataByteSize);
    return noErr;
}

} // namespace

void platform_audio_null_device(bool on) { g_null_device = on; }

bool platform_audio_open(Engine& e, const AudioDeviceDesc& desc) {
    KV_UNUSED(e);
    if (g_audio.unit) { KV_ERROR("audio", "a device is already open"); return false; }
    if (!desc.callback || desc.channels == 0 || desc.channels > 8 || desc.rate == 0) { KV_ERROR("audio", "bad device description"); return false; }
    @autoreleasepool {
        AVAudioSession* session = [AVAudioSession sharedInstance];
        NSError* err = nil;
        if (![session setCategory:AVAudioSessionCategoryAmbient error:&err])
            KV_WARN("audio", "audio session category: %s", err.localizedDescription.UTF8String);
        [session setPreferredSampleRate:(double)desc.rate error:nil];
        [session setPreferredIOBufferDuration:(double)desc.frames / (double)desc.rate error:nil];
        if (![session setActive:YES error:&err]) {
            KV_WARN("audio", "audio session: %s", err.localizedDescription.UTF8String);
            return false;
        }
    }
    AudioComponentDescription cd{};
    cd.componentType         = kAudioUnitType_Output;
    cd.componentSubType      = kAudioUnitSubType_RemoteIO;
    cd.componentManufacturer = kAudioUnitManufacturer_Apple;
    AudioComponent comp = AudioComponentFindNext(nullptr, &cd);
    if (!comp || AudioComponentInstanceNew(comp, &g_audio.unit) != noErr) {
        KV_WARN("audio", "no RemoteIO audio unit");
        g_audio.unit = nullptr;
        return false;
    }
    g_audio.desc = desc;
    g_audio.info.rate     = desc.rate;
    g_audio.info.channels = desc.channels;
    g_audio.info.frames   = desc.frames;
    g_audio.marked = false;
    // What the callback writes: the unit converts to the hardware's rate and layout.
    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate       = (f64)desc.rate;
    fmt.mFormatID         = kAudioFormatLinearPCM;
    fmt.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    fmt.mChannelsPerFrame = desc.channels;
    fmt.mBitsPerChannel   = 32;
    fmt.mFramesPerPacket  = 1;
    fmt.mBytesPerFrame    = desc.channels * 4;
    fmt.mBytesPerPacket   = desc.channels * 4;
    AURenderCallbackStruct cb{audio_render, &g_audio};
    OSStatus st = AudioUnitSetProperty(g_audio.unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fmt, sizeof fmt);
    if (st == noErr) st = AudioUnitSetProperty(g_audio.unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    if (st == noErr) st = AudioUnitInitialize(g_audio.unit);
    if (st == noErr) {
        UInt32 max_frames = 0, size = sizeof max_frames;
        if (AudioUnitGetProperty(g_audio.unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &max_frames, &size) == noErr && max_frames)
            g_audio.info.frames = max_frames < desc.frames ? max_frames : desc.frames;
        g_audio.info.open = true; // before the first callback can run
        st = AudioOutputUnitStart(g_audio.unit);
    }
    if (st != noErr) {
        KV_WARN("audio", "RemoteIO audio unit: error %d", (int)st);
        AudioComponentInstanceDispose(g_audio.unit);
        g_audio.unit = nullptr;
        g_audio.info = AudioDeviceInfo{};
        return false;
    }
    f64 hw = [AVAudioSession sharedInstance].sampleRate;
    KV_INFO("audio", "device open: %u Hz, %u channels, %u frames per buffer (hardware %.0f Hz), driver 'remoteio'%s",
            g_audio.info.rate, g_audio.info.channels, g_audio.info.frames, hw, g_null_device ? ", silent" : "");
    return true;
}

void platform_audio_close(Engine& e) {
    KV_UNUSED(e);
    if (!g_audio.unit) return;
    AudioOutputUnitStop(g_audio.unit); // returns once the callbacks have stopped
    AudioUnitUninitialize(g_audio.unit);
    AudioComponentInstanceDispose(g_audio.unit);
    g_audio.unit = nullptr;
    g_audio.info = AudioDeviceInfo{};
    KV_INFO("audio", "device closed");
}

const AudioDeviceInfo& platform_audio(Engine& e) {
    KV_UNUSED(e);
    return g_audio.info;
}

bool audio_backend_startup() { return true; }
void audio_backend_shutdown() {}

} // namespace kv
