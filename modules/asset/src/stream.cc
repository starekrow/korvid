#include "korvid/asset/stream.h"
#include "korvid/core/arena.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/platform/io.h"
#include "asset_internal.h"
// The decoder's declarations only; the implementation is stb_vorbis.cc.
#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_INTEGER_CONVERSION
#include "vendor/stb_vorbis.c"
#undef STB_VORBIS_HEADER_ONLY
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace kv {

struct StreamDecoder {
    stb_vorbis* v;
    u32         channels;
    f32         scratch[4096]; // a decoded block before it is made stereo
};

bool stream_loop_from_comments(const char* const* comments, u32 count, u64 frames, u64* start, u64* end) {
    bool have_start = false, have_len = false, have_end = false;
    u64 s = 0, len = 0, e = 0;
    for (u32 i = 0; i < count; ++i) {
        const char* c = comments[i];
        if (!strncasecmp(c, "LOOPSTART=", 10)) { s = strtoull(c + 10, nullptr, 10); have_start = true; }
        else if (!strncasecmp(c, "LOOPLENGTH=", 11)) { len = strtoull(c + 11, nullptr, 10); have_len = true; }
        else if (!strncasecmp(c, "LOOPEND=", 8)) { e = strtoull(c + 8, nullptr, 10); have_end = true; }
    }
    *start = 0;
    *end = frames;
    if (!have_start) return false;
    u64 stop = have_len ? s + len : have_end ? e : frames;
    if (s >= frames || stop <= s) return false;
    *start = s;
    *end = stop < frames ? stop : frames;
    return true;
}

StreamDecoder* stream_decoder_open(const StreamAsset& st, void* memory, usize memory_size) {
    if (!st.bytes || memory_size < sizeof(StreamDecoder) + 64 * 1024) return nullptr;
    StreamDecoder* d = (StreamDecoder*)memory;
    stb_vorbis_alloc work;
    work.alloc_buffer = (char*)memory + sizeof(StreamDecoder);
    work.alloc_buffer_length_in_bytes = (int)(memory_size - sizeof(StreamDecoder));
    int err = 0;
    d->v = stb_vorbis_open_memory(st.bytes, (int)st.size, &err, &work);
    if (!d->v) return nullptr;
    d->channels = (u32)stb_vorbis_get_info(d->v).channels;
    return d;
}

u32 stream_decoder_read(StreamDecoder* d, f32* out, u32 frames) {
    u32 ch = d->channels ? d->channels : 1;
    u32 done = 0;
    while (done < frames) {
        u32 want = frames - done;
        u32 room = (u32)(sizeof d->scratch / sizeof(f32)) / ch;
        if (want > room) want = room;
        int n = stb_vorbis_get_samples_float_interleaved(d->v, (int)ch, d->scratch, (int)(want * ch));
        if (n <= 0) break;
        for (int i = 0; i < n; ++i) {
            f32 l = d->scratch[i * ch], r = ch > 1 ? d->scratch[i * ch + 1] : l;
            out[(done + (u32)i) * 2] = l;
            out[(done + (u32)i) * 2 + 1] = r;
        }
        done += (u32)n;
    }
    return done;
}

bool stream_decoder_seek(StreamDecoder* d, u64 frame) { return stb_vorbis_seek(d->v, (unsigned)frame) != 0; }

void stream_decoder_close(StreamDecoder* d) {
    if (d && d->v) stb_vorbis_close(d->v); // frees nothing of ours: its memory was the caller's
    if (d) d->v = nullptr;
}

namespace {

void* stream_resolve(kv_handle h, void* ctx) { return ((AssetState_*)ctx)->streams.get(Handle{h}); }
int stream_describe(kv_handle h, void* ctx, char* out, size_t cap) {
    StreamAsset* s = ((AssetState_*)ctx)->streams.get(Handle{h});
    if (!s) return snprintf(out, cap, "stale");
    const char* st = s->state == AssetState::Ready ? "ready" : s->state == AssetState::Failed ? "failed" : "pending";
    return snprintf(out, cap, "'%s' %s %u Hz x%u %.1fs%s", s->path, st, s->rate, s->channels, s->seconds(), s->loop_tagged ? " loop-tagged" : "");
}

void fail_stream(Engine& e, AssetState_& s, Handle h, StreamAsset& st, AssetError err, const char* why) {
    st.state = AssetState::Failed;
    st.error = err;
    st.io    = Handle{};
    KV_WARN("asset", "stream '%s': %s%s%s", st.path, asset_error_name(err), why ? ": " : "", why ? why : "");
    e.events.emit(AssetFailed{h, err});
    (void)s;
}

// Opens the file once to learn what it is: rate, channels, length, loop points.
bool probe(StreamAsset& st, const char** why) {
    MemScope scope;
    stb_vorbis_alloc work;
    work.alloc_buffer_length_in_bytes = (int)KV_STREAM_DECODER_MEMORY;
    work.alloc_buffer = (char*)alloc_in(mem_scratch(), KV_STREAM_DECODER_MEMORY);
    int err = 0;
    stb_vorbis* v = stb_vorbis_open_memory(st.bytes, (int)st.size, &err, &work);
    if (!v) { *why = err == VORBIS_outofmem ? "the decoder needs more memory" : "not an Ogg Vorbis file"; return false; }
    stb_vorbis_info info = stb_vorbis_get_info(v);
    st.rate = (u32)info.sample_rate;
    st.channels = (u32)info.channels;
    st.frames = stb_vorbis_stream_length_in_samples(v);
    stb_vorbis_comment com = stb_vorbis_get_comment(v);
    st.loop_tagged = stream_loop_from_comments(com.comment_list, (u32)com.comment_list_length, st.frames, &st.loop_start, &st.loop_end);
    stb_vorbis_close(v);
    if (!st.frames || !st.rate) { *why = "an empty stream"; return false; }
    return true;
}

} // namespace

bool asset_streams_init(Engine&, AssetState_& s, ModuleInstance& self) {
    s.stream_by_path.init(&self.general);
    kv_handle_kind_desc desc{};
    desc.name = "asset.stream"; desc.type_key = type_key<StreamAsset>(); desc.ctx = &s;
    desc.resolve = stream_resolve; desc.describe = stream_describe; desc.flags = KV_HANDLE_GENERATIONAL;
    s.stream_kind = handle_kind_register(desc);
    if (!s.stream_kind) return false;
    s.streams.use_kind(s.stream_kind);
    return s.streams.init(256, self.general.tag);
}

void asset_streams_shutdown(Engine& e, AssetState_& s) {
    IoQueue* io = platform_io(e);
    for (u32 i = 0; i < s.streams.len; ++i) {
        if (!s.streams.is_alive(i)) continue;
        StreamAsset& st = s.streams.items[i];
        if (io && !st.io.is_null()) io->cancel(st.io);
        free_in(s.general, st.bytes, st.size ? st.size : 1);
    }
    kv_handle_kind_unregister(s.stream_kind);
    s.stream_by_path.free();
    s.streams.free();
}

void asset_streams_pump(TickContext& ctx) {
    AssetState_& s = ctx.state_as<AssetState_>();
    Engine& e = *ctx.engine;
    IoQueue* io = platform_io(e);
    if (!io) return;
    asset_reap_orphans(e, s, s.stream_kind);
    for (u32 i = 0; i < io->completions().len;) {
        IoCompletion c = io->completions()[i];
        if (!asset_is_kind(s, c.user, s.stream_kind)) { ++i; continue; }
        Handle h{c.user};
        StreamAsset* st = s.streams.get(h);
        if (!st || st->io != c.request) { ++i; continue; }
        io->reap(c.request);
        st->io = Handle{};
        if (c.status != IoStatus::Done) { fail_stream(e, s, h, *st, c.status == IoStatus::Failed && c.error == 2 ? AssetError::NotFound : AssetError::Read, nullptr); continue; }
        st->bytes = (u8*)c.data;
        st->size  = c.size;
        const char* why = nullptr;
        if (!probe(*st, &why)) { free_in(s.general, st->bytes, st->size ? st->size : 1); st->bytes = nullptr; st->size = 0; fail_stream(e, s, h, *st, AssetError::Decode, why); continue; }
        st->state = AssetState::Ready;
        KV_INFO("asset", "stream '%s' ready: %u Hz x%u, %.1f s, %llu KB compressed%s", st->path, st->rate, st->channels, st->seconds(),
                (unsigned long long)(st->size / 1024), st->loop_tagged ? ", loop points tagged" : "");
        e.events.emit(AssetReady{h});
    }
}

Handle stream_load(Engine& e, AssetState_& s, const char* path) {
    IoQueue* io = platform_io(e);
    if (!io || !path) { KV_ERROR("asset", "cannot load stream '%s': asset and platform must be up", path ? path : "?"); return Handle{}; }
    if (strlen(path) >= KV_ASSET_PATH_MAX) { KV_ERROR("asset", "path too long: '%s'", path); return Handle{}; }
    Name name = name_intern(path);
    if (const Handle* existing = s.stream_by_path.find(name)) return *existing;
    StreamAsset st{};
    strncpy(st.path, path, sizeof st.path - 1);
    Handle h = s.streams.create(st);
    if (h.is_null()) return Handle{};
    s.stream_by_path.set(name, h);
    IoRequest req;
    req.path  = path;
    req.alloc = s.general;
    req.user  = h.bits;
    Handle iq = io->read_asset(req);
    StreamAsset* rec = s.streams.get(h);
    if (iq.is_null()) { fail_stream(e, s, h, *rec, AssetError::Read, "could not submit"); return h; }
    rec->io = iq;
    return h;
}

void stream_retire(Engine& e, AssetState_& s, Handle h) {
    StreamAsset* st = s.streams.get(h);
    if (!st) return;
    IoQueue* io = platform_io(e);
    if (io && !st->io.is_null()) io->cancel(st->io);
    free_in(s.general, st->bytes, st->size ? st->size : 1); // nothing decodes it: voices hold references
    s.stream_by_path.remove(name_intern(st->path));
    KV_INFO("asset", "stream '%s' retired", st->path);
    s.streams.destroy(h);
}

Handle asset_load_stream(Engine& e, const char* path, Handle scope) {
    AssetState_* s = asset_state(e);
    if (!s) return Handle{};
    return asset_hold(*s, scope, stream_load(e, *s, path));
}

Handle asset_load_stream(Engine& e, const char* path) { return asset_load_stream(e, path, asset_scope_global(e)); }

const StreamAsset* asset_stream(Engine& e, Handle asset) {
    AssetState_* s = asset_state(e);
    return s ? s->streams.get(asset) : nullptr;
}

} // namespace kv
