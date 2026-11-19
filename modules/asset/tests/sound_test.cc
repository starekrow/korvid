// The sound decoders: a 16-bit stereo WAV built here decodes to the samples written, 8
// and 24-bit and float too, a bad file is refused; an Ogg Vorbis file from the system's
// sound theme decodes if one is installed (skipped otherwise).
#include "korvid/asset/sound.h"
#include "korvid/platform/io.h"
#include "korvid/core/memory.h"
#include "korvid/core/test.h"
#include "korvid/math/scalar.h"
#include <string.h>

namespace {

using namespace kv;

void put16(u8*& p, u16 v) { *p++ = (u8)v; *p++ = (u8)(v >> 8); }
void put32(u8*& p, u32 v) { put16(p, (u16)v); put16(p, (u16)(v >> 16)); }

// A WAV with `frames` frames of `channels` channels at `bits`; sample i of channel ch is
// value(i, ch) in [-1, 1].
usize make_wav(u8* out, u32 frames, u32 channels, u32 bits, bool is_float, f32 (*value)(u32, u32)) {
    u8* p = out;
    u32 bytes = bits / 8;
    u32 data = frames * channels * bytes;
    memcpy(p, "RIFF", 4); p += 4; put32(p, 36 + data); memcpy(p, "WAVE", 4); p += 4;
    memcpy(p, "fmt ", 4); p += 4; put32(p, 16); put16(p, is_float ? 3 : 1); put16(p, (u16)channels);
    put32(p, 22050); put32(p, 22050 * channels * bytes); put16(p, (u16)(channels * bytes)); put16(p, (u16)bits);
    memcpy(p, "data", 4); p += 4; put32(p, data);
    for (u32 i = 0; i < frames; ++i)
        for (u32 ch = 0; ch < channels; ++ch) {
            f32 v = value(i, ch);
            switch (bits) {
            case 8:  *p++ = (u8)(v * 127.0f + 128.0f); break;
            case 16: put16(p, (u16)(i16)(v * 32767.0f)); break;
            case 24: { i32 s = (i32)(v * 8388607.0f); *p++ = (u8)s; *p++ = (u8)(s >> 8); *p++ = (u8)(s >> 16); break; }
            default: if (is_float) { u32 u; memcpy(&u, &v, 4); put32(p, u); } else put32(p, (u32)(i32)(v * 2147483647.0f)); break;
            }
        }
    return (usize)(p - out);
}

f32 tone(u32 i, u32 ch) { return ch == 0 ? kv::sin((f32)i * 0.1f) * 0.9f : -0.5f; }

} // namespace

void test_sound(kv::TestCtx& c) {
    using namespace kv;
    c.begin("sound");
    static u8 buf[64 * 1024];
    const char* why = nullptr;
    const u32 bits_list[] = {8, 16, 24, 32};
    for (u32 bits : bits_list) {
        SoundAsset s{};
        usize n = make_wav(buf, 200, 2, bits, false, tone);
        KV_CHECK(c, sound_decode(buf, n, mem_general(), s, &why));
        KV_CHECK(c, s.rate == 22050 && s.channels == 2 && s.frames == 200 && s.samples);
        f32 tol = bits == 8 ? 0.02f : 0.001f;
        bool ok = true;
        for (u32 i = 0; i < 200 && ok; ++i) ok = kv::fabs(s.samples[i * 2] - tone(i, 0)) < tol && kv::fabs(s.samples[i * 2 + 1] + 0.5f) < tol;
        KV_CHECK(c, ok);
        free_in(mem_general(), s.samples, (usize)s.frames * s.channels * 4);
    }
    SoundAsset f{};
    usize n = make_wav(buf, 50, 1, 32, true, tone);
    KV_CHECK(c, sound_decode_wav(buf, n, mem_general(), f, &why) && f.channels == 1 && kv::fabs(f.samples[7] - tone(7, 0)) < 1e-6f);
    free_in(mem_general(), f.samples, (usize)f.frames * 4);
    SoundAsset bad{};
    KV_CHECK(c, !sound_decode("not a sound", 11, mem_general(), bad, &why) && why);
    memcpy(buf, "RIFF\0\0\0\0WAVEjunk", 16);
    KV_CHECK(c, !sound_decode(buf, 16, mem_general(), bad, &why));

    // Ogg: the freedesktop sound theme ships .oga files (Vorbis in Ogg); use one if present.
    const char* candidates[] = {"/usr/share/sounds/freedesktop/stereo/bell.oga", "/usr/share/sounds/freedesktop/stereo/message.oga"};
    for (const char* path : candidates) {
        IoBytes file;
        if (!io_read_now(path, &file)) continue;
        SoundAsset o{};
        bool ok = sound_decode(file.data, file.size, mem_general(), o, &why);
        KV_CHECK(c, ok && o.rate >= 8000 && o.channels >= 1 && o.frames > 1000);
        if (ok) {
            f32 peak = 0.0f;
            for (u32 i = 0; i < o.frames * o.channels; ++i) if (kv::fabs(o.samples[i]) > peak) peak = kv::fabs(o.samples[i]);
            KV_CHECK(c, peak > 0.01f && peak <= 1.0f);
            KV_INFO("test", "decoded %s: %u Hz x%u, %u frames, peak %.2f", path, o.rate, o.channels, o.frames, peak);
            free_in(mem_general(), o.samples, (usize)o.frames * o.channels * 4);
            // Cut short, it is refused (no page is marked last) or decodes shorter, with a
            // warning: never its whole length as if nothing were missing.
            const usize cuts[] = {file.size / 2, file.size - 3};
            for (usize cut : cuts) {
                SoundAsset t{};
                strcpy(t.path, "cut.ogg");
                why = nullptr;
                bool cut_ok = sound_decode(file.data, cut, mem_general(), t, &why);
                KV_CHECK(c, cut_ok ? t.frames < o.frames : why != nullptr);
                if (cut_ok) free_in(mem_general(), t.samples, (usize)t.frames * t.channels * 4);
            }
        }
        file.free();
        break;
    }
}
