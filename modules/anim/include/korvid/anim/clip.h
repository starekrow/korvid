// Korvid anim: clips. A clip is a duration, a loop mode and tracks; a track is a channel,
// an interpolation and keys at ascending times in [0, duration]. Clips are immutable
// once made and shared by every player of them; a handle names one (handle kind
// "anim.clip"). Two ways to get one, exactly as with materials:
//
//   anim_load(e, "mygame/flame.anim.json")   a file, read through the I/O queue and parsed
//                                            in "anim.pump"; AssetReady/AssetFailed follow
//   anim_create(e, desc)                     built now from a descriptor; Ready at once
//
// The file:
//   { "duration": 0.8, "loop": "loop",            loop: once | loop | pingpong
//     "tracks": [
//       { "channel": "uv", "interp": "step", "grid": [4, 1], "fps": 12, "frames": [0, 1, 2, 3] },
//       { "channel": "tint", "interp": "linear", "keys": [[0, [1, 1, 1, 1]], [0.8, [1, 1, 1, 0]]] },
//       { "channel": "position", "keys": [[0, [0, 0]], [0.4, [0, 10]]] },
//       { "channel": "event", "keys": [[0.4, "step"]] } ] }
//
// A uv track written as a grid expands to step keys at the fence posts (frame i at
// i / fps); a clip without a "duration" lasts as long as its longest track. Channels:
//
//   uv         rect2 (x, y, w, h in texture space)   step            Sprite.uv
//   tint       rgba                                   step or linear  Sprite.tint
//   alpha      a                                      step or linear  Sprite.tint.a, the color kept
//   position   x, y                                   step or linear  Local2 on a child ktable, else Transform2
//   rotation   radians                                step or linear  likewise
//   scale      x, y                                   step or linear  likewise
//   event      a name                                 fires on crossing   AnimEvent{entity, clip, name}
//
// The channel list has room; a skeleton later is more channels and a target per track.
#pragma once
#include "korvid/asset_render/texture.h"
#include "korvid/core/array.h"
#include "korvid/core/component.h"
#include "korvid/core/name.h"
#include "korvid/math/math.h"

namespace kv {

struct Engine;
struct SaveWriter;

enum class AnimChannel : u8 { Uv, Tint, Alpha, Position, Rotation, Scale, Event, Count };
enum class AnimInterp  : u8 { Step, Linear };
enum class AnimLoop    : u8 { Once, Loop, PingPong };

const char* anim_channel_name(AnimChannel c);
bool        anim_channel_parse(const char* s, AnimChannel* out);
const char* anim_loop_name(AnimLoop l);
bool        anim_loop_parse(const char* s, AnimLoop* out);

// One key: a time and a value in the channel's shape (unused lanes zero), or an event name.
struct AnimKey {
    f32  time = 0.0f;
    f32  v[4] = {};
    Name event = {};
};

struct AnimTrackDesc {
    AnimChannel         channel = AnimChannel::Uv;
    AnimInterp          interp  = AnimInterp::Step;
    Span<const AnimKey> keys    = {}; // ascending times
};

struct AnimClipDesc {
    const char*               name     = "";    // what saves and tools call it; copied. Empty: derived
                                                // from the content ("clip:<hash>")
    f32                       duration = 0.0f;  // 0: the last key's time across tracks
    AnimLoop                  loop     = AnimLoop::Once;
    Span<const AnimTrackDesc> tracks   = {};
};

// A track as stored: its keys are a run of the clip's key array.
struct AnimTrack {
    AnimChannel channel;
    AnimInterp  interp;
    u32         first = 0, count = 0;
};

struct AnimClip {
    char            path[KV_ASSET_PATH_MAX]; // the file, or the descriptor's name: what the clip is called
    AssetState      state = AssetState::Pending;
    AssetError      error = AssetError::None;
    Handle          io;                       // the pending read while loading
    f32             duration = 0.0f;
    AnimLoop        loop     = AnimLoop::Once;
    Array<AnimTrack> tracks;
    Array<AnimKey>   keys;
    u32             players  = 0;             // for the log: how many are on it now
    u32             refs     = 0;             // scopes and assets holding it (a sheet its clips, a map its tiles')
};

// Clips are assets of the asset module's scopes (anim.clip is registered with
// asset_kind_register): a scope holds a clip, a clip that nothing holds retires at the
// next asset pump. The forms without a scope use the global scope; a null scope means the
// caller holds the clip with asset_ref (a sheet its clips, a map its tiles').
//
// Starts loading <exe dir>/assets/<path>; the handle comes back at once and the clip is
// Pending until anim.pump parses it. The same path is the same handle.
Handle          anim_load(Engine& e, const char* path, Handle scope);
Handle          anim_load(Engine& e, const char* path);
// Builds a clip now. Null, with the reason logged, for a malformed descriptor (keys out
// of order, a value past the duration, an event on a non-event channel).
Handle          anim_create(Engine& e, const AnimClipDesc& desc, Handle scope);
Handle          anim_create(Engine& e, const AnimClipDesc& desc);
// The clip, or null if the handle is stale or not a clip.
const AnimClip* anim_clip(Engine& e, Handle clip);
// The live clip with that name (its path, its descriptor's name, or the derived one), or null.
Handle          anim_find(Engine& e, const char* name);

// The file format on its own. Fills `out` (its arrays allocate from `alloc`) from the
// text; false with the reason and byte offset on a bad file.
bool anim_parse(const char* text, usize len, Allocator* alloc, AnimClip& out, const char** error, u32* error_at);
// A descriptor into a clip record; false with the reason on a bad one.
bool anim_build(const AnimClipDesc& desc, Allocator* alloc, AnimClip& out, const char** error);

// Writes a clip as the file format: duration, loop, and every track as keys (a flipbook as
// uv keys, not as a grid). anim_parse of the text builds a clip equal to this one, key
// for key.
bool anim_write(const AnimClip& clip, SaveWriter& w);

// Samples a value track at `time` (clamped to the keys' span) into `out`: step holds the
// last key at or before the time, linear blends with the next. False for an event track
// or an empty one.
bool anim_sample(const AnimClip& clip, const AnimTrack& track, f32 time, f32 out[4]);

} // namespace kv

// Name is an interned u64; events carry one.
template <> struct kv::FieldTraits<kv::Name> : kv::FieldTraits<kv::u64> {};
