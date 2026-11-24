#include "korvid/asset/sound.h"
#include "korvid/core/format.h"
#include "korvid/asset/module.h"
#include "korvid/core/bytes.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/platform/io.h"
#include "asset_internal.h"
#include <string.h>

// The decoder's declarations only; the implementation is stb_vorbis.cc.
#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_INTEGER_CONVERSION
#include "vendor/stb_vorbis.c"
#undef STB_VORBIS_HEADER_ONLY

namespace kv {

// ---- WAV ---------------------------------------------------------------------------------------

bool sound_decode_wav(const void* bytes, usize size, Allocator* alloc, SoundAsset& out, const char** error) {
    const u8* p = (const u8*)bytes;
    if (size < 12 || memcmp(p, "RIFF", 4) != 0 || memcmp(p + 8, "WAVE", 4) != 0) { *error = "not a RIFF WAVE file"; return false; }
    u16 format = 0, channels = 0, bits = 0;
    u32 rate = 0;
    const u8* data = nullptr;
    u32 data_size = 0;
    usize at = 12;
    while (at + 8 <= size) {
        const u8* chunk = p + at;
        u32 len = load_le32(chunk + 4);
        const u8* body = chunk + 8;
        usize avail = size - (at + 8);
        if (len > avail) len = (u32)avail; // a truncated last chunk: take what is there
        if (memcmp(chunk, "fmt ", 4) == 0) {
            if (len < 16) { *error = "fmt chunk too short"; return false; }
            format   = load_le16(body);
            channels = load_le16(body + 2);
            rate     = load_le32(body + 4);
            bits     = load_le16(body + 14);
            if (format == 0xFFFE && len >= 26) format = load_le16(body + 24); // WAVE_FORMAT_EXTENSIBLE: the sub-format's first word
        } else if (memcmp(chunk, "data", 4) == 0) {
            data = body;
            data_size = len;
        }
        at += 8 + len + (len & 1);
    }
    if (!data || !rate || !channels) { *error = "no fmt or data chunk"; return false; }
    bool is_float = format == 3;
    if (!(format == 1 || is_float)) { *error = "only PCM and float WAV are read (no ADPCM, no law)"; return false; }
    if (bits != 8 && bits != 16 && bits != 24 && bits != 32) { *error = "sample size must be 8, 16, 24 or 32 bits"; return false; }
    if (is_float && bits != 32) { *error = "float WAV must be 32-bit"; return false; }
    u32 bytes_per = bits / 8;
    u32 frames = data_size / (bytes_per * channels);
    f32* samples = (f32*)alloc_in(alloc, (usize)frames * channels * sizeof(f32));
    if (!samples && frames) { *error = "out of memory"; return false; }
    const u8* s = data;
    for (usize i = 0; i < (usize)frames * channels; ++i, s += bytes_per) {
        f32 v;
        switch (bits) {
        case 8:  v = ((f32)s[0] - 128.0f) / 128.0f; break;
        case 16: v = (f32)(i16)load_le16(s) / 32768.0f; break;
        case 24: v = (f32)((i32)(((u32)s[0] << 8) | ((u32)s[1] << 16) | ((u32)s[2] << 24)) >> 8) / 8388608.0f; break;
        default:
            if (is_float) { u32 u = load_le32(s); memcpy(&v, &u, 4); }
            else v = (f32)(i32)load_le32(s) / 2147483648.0f;
            break;
        }
        samples[i] = v;
    }
    out.rate = rate; out.channels = channels; out.frames = frames; out.samples = samples;
    return true;
}

// ---- Ogg Vorbis ---------------------------------------------------------------------------------

bool sound_decode_ogg(const void* bytes, usize size, Allocator* alloc, SoundAsset& out, const char** error) {
    MemScope scope;
    // The decoder's own working memory, from scratch: it never calls malloc with this set.
    stb_vorbis_alloc work;
    work.alloc_buffer_length_in_bytes = 512 * 1024;
    work.alloc_buffer = (char*)alloc_in(mem_scratch(), (usize)work.alloc_buffer_length_in_bytes);
    int err = 0;
    stb_vorbis* v = stb_vorbis_open_memory((const unsigned char*)bytes, (int)size, &err, &work);
    if (!v) { *error = err == VORBIS_outofmem ? "vorbis decoder needs more working memory" : "not an Ogg Vorbis file"; return false; }
    stb_vorbis_info info = stb_vorbis_get_info(v);
    u32 channels = (u32)info.channels;
    // The length is the last page's position. A file cut short, or with its last page
    // damaged, has no page marked last: no length at all, or an earlier page's.
    u32 total = stb_vorbis_stream_length_in_samples(v); // frames
    if (!total && stb_vorbis_get_error(v) == VORBIS_cant_find_last_page) { stb_vorbis_close(v); *error = "no Ogg page is marked last: the file is cut short or corrupt"; return false; }
    f32* samples = (f32*)alloc_in(alloc, (usize)total * channels * sizeof(f32));
    if (!samples && total) { stb_vorbis_close(v); *error = "out of memory"; return false; }
    u32 got = 0;
    while (got < total) {
        int n = stb_vorbis_get_samples_float_interleaved(v, (int)channels, samples + (usize)got * channels, (int)((total - got) * channels));
        if (n <= 0) break;
        got += (u32)n;
    }
    f32 more[256];
    bool past_end = got == total && stb_vorbis_get_samples_float_interleaved(v, (int)channels, more, (int)KV_COUNTOF(more)) > 0;
    stb_vorbis_close(v);
    const char* name = out.path[0] ? out.path : "(unnamed)";
    if (got < total) { // the samples were allocated for `total` frames, as they are freed: silence the rest
        memset(samples + (usize)got * channels, 0, (usize)(total - got) * channels * sizeof(f32));
        KV_WARN("asset", "sound '%s': Ogg Vorbis stops decoding at frame %u of %u: the file is corrupt or cut short; the rest is silence", name, got, total);
    } else if (past_end) {
        KV_WARN("asset", "sound '%s': Ogg Vorbis decodes past its length, %u frames: the file is cut short or corrupt; the sound ends there", name, total);
    }
    out.rate = (u32)info.sample_rate; out.channels = channels; out.frames = total; out.samples = samples;
    return true;
}

bool sound_decode(const void* bytes, usize size, Allocator* alloc, SoundAsset& out, const char** error) {
    const u8* p = (const u8*)bytes;
    if (size >= 4 && memcmp(p, "RIFF", 4) == 0) return sound_decode_wav(bytes, size, alloc, out, error);
    if (size >= 4 && memcmp(p, "OggS", 4) == 0) return sound_decode_ogg(bytes, size, alloc, out, error);
    *error = "neither a WAV nor an Ogg file";
    return false;
}

// ---- the records -------------------------------------------------------------------------------

namespace {

void* sound_resolve(kv_handle h, void* ctx) { return ((AssetState_*)ctx)->sounds.get(Handle{h}); }
const char* sound_name_of(kv_handle h, void* ctx) { auto* a = ((AssetState_*)ctx)->sounds.get(Handle{h}); return a ? a->path : nullptr; }
kv_handle sound_find_name(const char* name, void* ctx) {
    const Handle* h = ((AssetState_*)ctx)->sound_by_path.find(name_lookup(name));
    return h ? h->bits : 0;
}
int sound_describe(kv_handle h, void* ctx, char* out, size_t cap) {
    SoundAsset* s = ((AssetState_*)ctx)->sounds.get(Handle{h});
    if (!s) return format(out, cap, "stale");
    const char* st = s->state == AssetState::Ready ? "ready" : s->state == AssetState::Failed ? "failed" : "pending";
    return format(out, cap, "'%s' %s %u Hz x%u %.2fs", s->path, st, s->rate, s->channels, s->seconds());
}

void fail_sound(Engine& e, AssetState_& s, Handle h, SoundAsset& snd, AssetError err, const char* detail) {
    snd.state = AssetState::Failed;
    snd.error = err;
    snd.io    = Handle{};
    ++s.sounds_failed;
    KV_WARN("asset", "sound '%s': %s%s%s", snd.path, asset_error_name(err), detail ? ": " : "", detail ? detail : "");
    e.events.emit(AssetFailed{h, err});
}

} // namespace

bool asset_sounds_init(Engine&, AssetState_& s, ModuleInstance& self) {
    s.sound_by_path.init(&self.general);
    kv_handle_kind_desc desc{};
    desc.name = "asset.sound"; desc.type_key = type_key<SoundAsset>(); desc.ctx = &s;
    desc.resolve = sound_resolve; desc.name_of = sound_name_of; desc.find_name = sound_find_name; desc.describe = sound_describe; desc.flags = KV_HANDLE_GENERATIONAL;
    s.sound_kind = handle_kind_register(desc);
    if (!s.sound_kind) return false;
    s.sounds.use_kind(s.sound_kind);
    return s.sounds.init(4096, self.general.tag);
}

void asset_sounds_shutdown(Engine& e, AssetState_& s) {
    IoQueue* io = platform_io(e);
    for (u32 i = 0; i < s.sounds.len; ++i) {
        if (!s.sounds.is_alive(i)) continue;
        SoundAsset& snd = s.sounds.items[i];
        if (io && !snd.io.is_null()) io->cancel(snd.io);
        free_in(s.general, snd.samples, (usize)snd.frames * snd.channels * sizeof(f32));
    }
    KV_INFO("asset", "%u sounds loaded, %u failed", s.sounds_loaded, s.sounds_failed);
    kv_handle_kind_unregister(s.sound_kind);
    s.sound_by_path.free();
    s.sounds.free();
}

// Phase PreSim, main thread: sound files that arrived are decoded.
void asset_sounds_pump(TickContext& ctx) {
    AssetState_& s = ctx.state_as<AssetState_>();
    Engine& e = *ctx.engine;
    IoQueue* io = platform_io(e);
    if (!io) return;
    asset_reap_orphans(e, s, s.sound_kind);
    for (u32 i = 0; i < io->completions().len;) {
        IoCompletion c = io->completions()[i];
        if (!asset_is_kind(s, c.user, s.sound_kind)) { ++i; continue; }
        Handle h{c.user};
        SoundAsset* snd = s.sounds.get(h);
        if (!snd || snd->io != c.request) { ++i; continue; }
        io->reap(c.request);
        snd->io = Handle{};
        if (c.status != IoStatus::Done) {
            fail_sound(e, s, h, *snd, c.status == IoStatus::Failed && c.error == 2 ? AssetError::NotFound : AssetError::Read, nullptr);
            continue;
        }
        const char* why = nullptr;
        bool ok = sound_decode(c.bytes.data, c.bytes.size, s.general, *snd, &why);
        c.bytes.free();
        if (!ok) { fail_sound(e, s, h, *snd, AssetError::Decode, why); continue; }
        snd->state = AssetState::Ready;
        ++s.sounds_loaded;
        KV_INFO("asset", "sound '%s' ready: %u Hz x%u, %.2f s", snd->path, snd->rate, snd->channels, snd->seconds());
        e.events.emit(AssetReady{h});
    }
}

Handle asset_load_sound(Engine& e, const char* path, Handle scope) {
    AssetState_* s = asset_state(e);
    if (!s) { KV_ERROR("asset", "cannot load sound '%s': the asset module is not up", path ? path : "?"); return Handle{}; }
    return asset_hold(*s, scope, sound_load(e, *s, path));
}

Handle asset_load_sound(Engine& e, const char* path) { return asset_load_sound(e, path, asset_scope_global(e)); }

void sound_retire(Engine& e, AssetState_& s, Handle h) {
    SoundAsset* snd = s.sounds.get(h);
    if (!snd) return;
    IoQueue* io = platform_io(e);
    if (io && !snd->io.is_null()) io->cancel(snd->io);
    asset_bury_samples(s, snd->samples, (usize)snd->frames * snd->channels * sizeof(f32));
    s.sound_by_path.remove(name_intern(snd->path));
    KV_INFO("asset", "sound '%s' retired", snd->path);
    s.sounds.destroy(h); // stale from here; the samples go when the mixer says
}

Handle sound_load(Engine& e, AssetState_& st, const char* path) {
    AssetState_* s = &st;
    IoQueue* io = platform_io(e);
    if (!io || !path) { KV_ERROR("asset", "cannot load sound '%s': asset and platform must be up", path ? path : "?"); return Handle{}; }
    if (strlen(path) >= KV_ASSET_PATH_MAX) { KV_ERROR("asset", "path too long: '%s'", path); return Handle{}; }
    Name name = name_intern(path);
    if (const Handle* existing = s->sound_by_path.find(name)) return *existing;

    SoundAsset snd{};
    strncpy(snd.path, path, sizeof snd.path - 1);
    snd.state = AssetState::Pending;
    Handle h = s->sounds.create(snd);
    if (h.is_null()) return Handle{};
    s->sound_by_path.set(name, h);

    IoRequest req;
    req.path  = path;
    req.alloc = s->general;
    req.user  = h.bits;
    Handle iq = io->read_asset(req);
    SoundAsset* rec = s->sounds.get(h);
    if (iq.is_null()) { fail_sound(e, *s, h, *rec, AssetError::Read, "could not submit"); return h; }
    rec->io = iq;
    return h;
}

const SoundAsset* asset_sound(Engine& e, Handle asset) {
    AssetState_* s = asset_state(e);
    return s ? s->sounds.get(asset) : nullptr;
}

} // namespace kv
