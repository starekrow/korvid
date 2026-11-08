// Korvid asset: sounds as assets. asset_load_sound(e, "mygame/shot.wav") returns a handle
// at once; the file reads through the I/O queue and "asset.pump_sounds" (phase PreSim,
// main thread) decodes it, WAV (PCM 8, 16, 24 and 32-bit integer and 32-bit float, any
// channel count) with our own reader or Ogg Vorbis with stb_vorbis, into interleaved f32
// samples at the file's own rate; AssetReady follows. The samples live until shutdown,
// so the mixer's thread may read them without a lock once the record is Ready; a voice
// started before that waits for it. Nothing resamples at load: the mixer reads at the
// ratio of the file's rate to the device's.
#pragma once
#include "korvid/asset/texture.h"
#include "korvid/core/memory.h"

namespace kv {

struct SoundAsset {
    char       path[KV_ASSET_PATH_MAX];
    AssetState state = AssetState::Pending;
    AssetError error = AssetError::None;     // Decode for a file neither reader accepts
    Handle     io;
    u32        rate     = 0;                 // frames per second
    u32        channels = 0;                 // 1 or 2 are what the mixer pans; more are summed
    u32        frames   = 0;
    f32*       samples  = nullptr;           // frames * channels, interleaved, [-1, 1]
    f64        seconds() const { return rate ? (f64)frames / (f64)rate : 0.0; }
};

Handle            asset_load_sound(Engine& e, const char* path);
const SoundAsset* asset_sound(Engine& e, Handle asset);

// The decoders on their own, for tools and tests. Both allocate `out.samples` from
// `alloc` (frames * channels * 4 bytes) and fill rate, channels and frames; false with
// the reason on a file they cannot read. sound_decode picks by the bytes' magic.
bool sound_decode_wav(const void* bytes, usize size, Allocator* alloc, SoundAsset& out, const char** error);
bool sound_decode_ogg(const void* bytes, usize size, Allocator* alloc, SoundAsset& out, const char** error);
bool sound_decode(const void* bytes, usize size, Allocator* alloc, SoundAsset& out, const char** error);

} // namespace kv
