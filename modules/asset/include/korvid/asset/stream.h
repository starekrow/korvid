// Korvid asset: streams. A long sound (music, ambience) kept compressed in memory and
// decoded as it plays, never whole. asset_load_stream(e, "game/town.ogg", scope) reads the
// Ogg Vorbis file through the mounts, checks it opens, and fills in the rate, the channels,
// the length and the loop points; the audio module decodes it ahead of the mixer with a
// StreamDecoder per voice. Loop points come from the Vorbis comments RPG tools write:
// LOOPSTART and LOOPLENGTH (or LOOPEND), in frames; without them the whole track loops.
// A voice playing a stream holds a reference on it, so a stream never retires under one.
#pragma once
#include "korvid/asset/texture.h"

namespace kv {

struct StreamAsset {
    char       path[KV_ASSET_PATH_MAX];
    AssetState state = AssetState::Pending;
    AssetError error = AssetError::None;
    Handle     io;
    u8*        bytes = nullptr;  // the file, once Ready
    usize      size  = 0;
    u32        rate = 0, channels = 0;
    u64        frames = 0;
    u64        loop_start = 0, loop_end = 0; // [start, end) in frames; the whole track without tags
    bool       loop_tagged = false;
    u32        refs = 0;
    f64        seconds() const { return rate ? (f64)frames / (f64)rate : 0.0; }
};

Handle             asset_load_stream(Engine& e, const char* path, Handle scope);
Handle             asset_load_stream(Engine& e, const char* path);
const StreamAsset* asset_stream(Engine& e, Handle asset);

// Decoding, for the audio module on the main thread. The decoder lives in `memory` (the
// caller's, at least KV_STREAM_DECODER_MEMORY bytes, kept until close) and never allocates.
constexpr usize KV_STREAM_DECODER_MEMORY = 512 * 1024;
struct StreamDecoder;
StreamDecoder* stream_decoder_open(const StreamAsset& s, void* memory, usize memory_size);
// Up to `frames` frames as interleaved stereo (a mono stream duplicated); 0 at the end,
// or where a file cut short or corrupt stops decoding, which warns once, naming the stream.
u32            stream_decoder_read(StreamDecoder* d, f32* stereo, u32 frames);
bool           stream_decoder_seek(StreamDecoder* d, u64 frame);
void           stream_decoder_close(StreamDecoder* d);
// The loop points of a list of Vorbis comments ("LOOPSTART=441000", ...), for tests.
bool           stream_loop_from_comments(const char* const* comments, u32 count, u64 frames, u64* start, u64* end);

} // namespace kv
