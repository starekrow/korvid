#include "korvid/anim/clip.h"
#include "korvid/core/json.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/save/writer.h"
#include <math.h>
#include <string.h>

namespace kv {

namespace {
const char* const k_channel_names[(u32)AnimChannel::Count] = {"uv", "tint", "alpha", "position", "rotation", "scale", "event"};
const char* const k_loop_names[3] = {"once", "loop", "pingpong"};
const u32         k_channel_lanes[(u32)AnimChannel::Count] = {4, 4, 1, 2, 1, 2, 0};
} // namespace

const char* anim_channel_name(AnimChannel c) { return (u32)c < (u32)AnimChannel::Count ? k_channel_names[(u32)c] : "?"; }
bool anim_channel_parse(const char* s, AnimChannel* out) {
    for (u32 i = 0; i < (u32)AnimChannel::Count; ++i)
        if (strcmp(s, k_channel_names[i]) == 0) { *out = (AnimChannel)i; return true; }
    return false;
}
const char* anim_loop_name(AnimLoop l) { return (u32)l < 3 ? k_loop_names[(u32)l] : "?"; }
bool anim_loop_parse(const char* s, AnimLoop* out) {
    for (u32 i = 0; i < 3; ++i)
        if (strcmp(s, k_loop_names[i]) == 0) { *out = (AnimLoop)i; return true; }
    return false;
}
u32 anim_channel_lanes(AnimChannel c) { return (u32)c < (u32)AnimChannel::Count ? k_channel_lanes[(u32)c] : 0; }

bool anim_build(const AnimClipDesc& desc, Allocator* alloc, AnimClip& out, const char** error) {
    out = AnimClip{};
    strncpy(out.path, desc.name ? desc.name : "", sizeof out.path - 1);
    out.tracks.alloc = out.keys.alloc = alloc;
    out.loop = desc.loop;
    f32 last = 0.0f;
    for (const AnimTrackDesc& td : desc.tracks) {
        if ((u32)td.channel >= (u32)AnimChannel::Count) { *error = "unknown channel"; return false; }
        bool is_event = td.channel == AnimChannel::Event;
        if (td.interp == AnimInterp::Linear && (td.channel == AnimChannel::Uv || is_event)) { *error = "uv and event tracks are step"; return false; }
        AnimTrack t;
        t.channel = td.channel;
        t.interp  = td.interp;
        t.first   = out.keys.len;
        t.count   = td.keys.len;
        f32 prev = -1.0f;
        for (const AnimKey& k : td.keys) {
            if (!(k.time >= 0.0f)) { *error = "a key time is negative"; return false; }
            if (k.time < prev || (!is_event && k.time == prev)) { *error = "keys must be in ascending time order"; return false; }
            if (is_event && k.event.is_null()) { *error = "an event key needs a name"; return false; }
            prev = k.time;
            if (k.time > last) last = k.time;
            out.keys.push(k);
        }
        out.tracks.push(t);
    }
    out.duration = desc.duration > 0.0f ? desc.duration : last;
    if (last > out.duration + 1e-4f) { *error = "a key is past the duration"; return false; }
    out.state = AssetState::Ready;
    return true;
}

bool anim_parse(const char* text, usize len, Allocator* alloc, AnimClip& out, const char** error, u32* error_at) {
    MemScope scope;
    JsonDoc doc;
    *error_at = 0;
    if (!json_parse(text, len, mem_scratch(), doc)) { *error = doc.error; *error_at = doc.error_at; return false; }
    const JsonValue& root = doc.root();
    if (!root.is_object()) { *error = "a clip file is one object"; return false; }

    AnimClipDesc desc;
    f32 grid_end = 0.0f; // the longest flipbook's end, one frame past its last key
    Array<AnimTrackDesc> tracks = array_in<AnimTrackDesc>(mem_scratch());
    Array<AnimKey>       keys   = array_in<AnimKey>(mem_scratch()); // every track's, in order; spans are fixed up after
    Array<u32>           starts = array_in<u32>(mem_scratch());
    for (const JsonValue* m = json_first(doc, root); m; m = json_next(doc, *m)) {
        if (strcmp(m->key, "duration") == 0) {
            if (!m->is_number() || m->number < 0.0) { *error = "duration must be a non-negative number"; return false; }
            desc.duration = (f32)m->number;
        } else if (strcmp(m->key, "loop") == 0) {
            if (!m->is_string() || !anim_loop_parse(m->str, &desc.loop)) { *error = "loop must be once, loop or pingpong"; return false; }
        } else if (strcmp(m->key, "tracks") == 0) {
            if (!m->is_array()) { *error = "tracks must be an array"; return false; }
            for (const JsonValue* tv = json_first(doc, *m); tv; tv = json_next(doc, *tv)) {
                if (!tv->is_object()) { *error = "a track is an object"; return false; }
                AnimTrackDesc td;
                const char* ch = json_string(json_get(doc, *tv, "channel"), nullptr);
                if (!ch || !anim_channel_parse(ch, &td.channel)) { *error = "track channel must be uv, tint, alpha, position, rotation, scale or event"; return false; }
                const char* in = json_string(json_get(doc, *tv, "interp"), "step");
                if (strcmp(in, "step") == 0) td.interp = AnimInterp::Step;
                else if (strcmp(in, "linear") == 0) td.interp = AnimInterp::Linear;
                else { *error = "interp must be step or linear"; return false; }
                u32 lanes = anim_channel_lanes(td.channel);
                starts.push(keys.len);
                if (const JsonValue* grid = json_get(doc, *tv, "grid")) {
                    // A flipbook: frames of a cols x rows sheet at fps, as uv step keys.
                    if (td.channel != AnimChannel::Uv) { *error = "grid is for uv tracks"; return false; }
                    f32 dims[2] = {0, 0};
                    if (json_numbers(doc, grid, dims, 2) != 2 || dims[0] < 1 || dims[1] < 1) { *error = "grid must be [cols, rows]"; return false; }
                    f32 fps = (f32)json_number(json_get(doc, *tv, "fps"), 0.0);
                    if (!(fps > 0.0f)) { *error = "a grid track needs fps > 0"; return false; }
                    const JsonValue* frames = json_get(doc, *tv, "frames");
                    if (!frames || !frames->is_array() || frames->count == 0) { *error = "a grid track needs frames"; return false; }
                    u32 cols = (u32)dims[0], rows = (u32)dims[1], i = 0;
                    for (const JsonValue* fv = json_first(doc, *frames); fv; fv = json_next(doc, *fv), ++i) {
                        if (!fv->is_number() || fv->number < 0 || fv->number >= cols * rows) { *error = "a frame index is outside the grid"; return false; }
                        u32 f = (u32)fv->number;
                        AnimKey k;
                        k.time = (f32)i / fps;
                        k.v[0] = (f32)(f % cols) / (f32)cols; k.v[1] = (f32)(f / cols) / (f32)rows;
                        k.v[2] = 1.0f / (f32)cols;            k.v[3] = 1.0f / (f32)rows;
                        keys.push(k);
                    }
                    // The last frame lasts a frame too: the track's end is one more fence post.
                    f32 end = (f32)i / fps;
                    if (end > grid_end) grid_end = end;
                } else {
                    const JsonValue* kv_ = json_get(doc, *tv, "keys");
                    if (!kv_ || !kv_->is_array()) { *error = "a track needs keys or a grid"; return false; }
                    for (const JsonValue* k2 = json_first(doc, *kv_); k2; k2 = json_next(doc, *k2)) {
                        if (!k2->is_array() || k2->count != 2) { *error = "a key is [time, value]"; return false; }
                        const JsonValue* tval = json_at(doc, *k2, 0);
                        const JsonValue* vval = json_at(doc, *k2, 1);
                        if (!tval->is_number()) { *error = "a key time is a number"; return false; }
                        AnimKey k;
                        k.time = (f32)tval->number;
                        if (td.channel == AnimChannel::Event) {
                            if (!vval->is_string() || !vval->len) { *error = "an event key's value is a name"; return false; }
                            k.event = name_intern(vval->str);
                        } else if (vval->is_number() && lanes == 1) {
                            k.v[0] = (f32)vval->number;
                        } else {
                            u32 n = json_numbers(doc, vval, k.v, 4);
                            if (!vval->is_array() || n != lanes) { *error = "a key value has the wrong number of lanes for its channel"; return false; }
                        }
                        keys.push(k);
                    }
                }
                tracks.push(td);
            }
        } else {
            *error = "unknown field"; return false;
        }
    }
    for (u32 i = 0; i < tracks.len; ++i) {
        u32 end = i + 1 < tracks.len ? starts[i + 1] : keys.len;
        tracks[i].keys = Span<const AnimKey>(keys.data + starts[i], end - starts[i]);
    }
    desc.tracks = tracks.span();
    // No duration in the file: the clip lasts as long as its longest track, a flipbook's
    // last frame included.
    if (desc.duration == 0.0f) {
        f32 last = grid_end;
        for (const AnimKey& k : keys) if (k.time > last) last = k.time;
        desc.duration = last;
    }
    return anim_build(desc, alloc, out, error);
}

bool anim_write(const AnimClip& clip, SaveWriter& w) {
    w.begin_object();
    w.key("duration"); w.f32_value(clip.duration);
    w.key("loop");     w.string_value(anim_loop_name(clip.loop));
    w.key("tracks");
    w.begin_array();
    for (const AnimTrack& t : clip.tracks) {
        w.begin_object();
        w.key("channel"); w.string_value(anim_channel_name(t.channel));
        w.key("interp");  w.string_value(t.interp == AnimInterp::Linear ? "linear" : "step");
        w.key("keys");
        w.begin_array();
        u32 lanes = anim_channel_lanes(t.channel);
        for (u32 i = 0; i < t.count; ++i) {
            const AnimKey& k = clip.keys[t.first + i];
            w.begin_array(true);
            w.f32_value(k.time);
            if (t.channel == AnimChannel::Event) w.string_value(name_str(k.event));
            else if (lanes == 1) w.f32_value(k.v[0]);
            else {
                w.begin_array(true);
                for (u32 l = 0; l < lanes; ++l) w.f32_value(k.v[l]);
                w.end_array();
            }
            w.end_array();
        }
        w.end_array();
        w.end_object();
    }
    w.end_array();
    w.end_object();
    return !w.failed;
}

bool anim_sample(const AnimClip& clip, const AnimTrack& track, f32 time, f32 out[4]) {
    if (track.channel == AnimChannel::Event || track.count == 0) return false;
    const AnimKey* keys = clip.keys.data + track.first;
    u32 n = track.count;
    if (time <= keys[0].time || n == 1) { memcpy(out, keys[0].v, sizeof keys[0].v); return true; }
    if (time >= keys[n - 1].time) { memcpy(out, keys[n - 1].v, sizeof keys[0].v); return true; }
    // The last key at or before `time`: binary search over ascending times.
    u32 lo = 0, hi = n - 1;
    while (hi - lo > 1) {
        u32 mid = (lo + hi) / 2;
        if (keys[mid].time <= time) lo = mid; else hi = mid;
    }
    if (track.interp == AnimInterp::Step) { memcpy(out, keys[lo].v, sizeof keys[0].v); return true; }
    f32 span = keys[hi].time - keys[lo].time;
    f32 t = span > 0.0f ? (time - keys[lo].time) / span : 0.0f;
    for (u32 i = 0; i < 4; ++i) out[i] = keys[lo].v[i] + (keys[hi].v[i] - keys[lo].v[i]) * t;
    return true;
}

} // namespace kv
