// JPEG decoding (KR25) against Pillow's decodes on libjpeg-turbo (jpeg_data.h, from
// make_jpeg_data.py): baseline at three samplings, greyscale, progressive, restart markers,
// CMYK, EXIF orientation; at full size and at a half, a quarter and an eighth. Then what a
// hostile file does: truncated, spoiled, too large, not a JPEG, and a mutation fuzzer.
// With KV_JPEG_BENCH naming a file, its decode is timed whole and at an eighth.
#include "korvid/image/jpeg.h"
#include "korvid/core/log.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include "jpeg_data.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

using namespace kv;

struct Diff { u32 max = 0; f64 mean = 0; };

Diff compare(const JpegImage& img, const unsigned char* rgb, u32 w, u32 h) {
    Diff d;
    if (img.width != w || img.height != h || !img.rgba) { d.max = 999; return d; }
    u64 sum = 0;
    for (u32 i = 0; i < w * h; ++i)
        for (u32 c = 0; c < 3; ++c) {
            i32 v = (i32)img.rgba[i * 4 + c] - (i32)rgb[i * 3 + c];
            u32 a = (u32)(v < 0 ? -v : v);
            sum += a;
            if (a > d.max) d.max = a;
        }
    d.mean = (f64)sum / ((f64)w * h * 3);
    return d;
}

struct Case { const char* name; const unsigned char* data; usize size; const unsigned char* rgb; u32 w, h; };

#define CASE(n) Case{#n, k_jpeg_##n, sizeof k_jpeg_##n, k_jpeg_##n##_rgb, k_jpeg_##n##_w, k_jpeg_##n##_h}
#define SCALED(n, s) Case{#n "/" #s, k_jpeg_##n, sizeof k_jpeg_##n, k_jpeg_##n##_##s##_rgb, k_jpeg_##n##_##s##_w, k_jpeg_##n##_##s##_h}

bool decode(const Case& c, u32 scale, JpegImage& out, bool orient = true) {
    JpegOptions o;
    o.scale = scale;
    o.orient = orient;
    const char* why = nullptr;
    bool ok = jpeg_decode(c.data, c.size, o, nullptr, out, &why);
    if (!ok) KV_ERROR("test", "jpeg %s: %s", c.name, why ? why : "?");
    return ok;
}

void release(JpegImage& img) { if (img.rgba) mem_free(img.rgba); img.rgba = nullptr; }

} // namespace

void test_jpeg(kv::TestCtx& c) {
    using namespace kv;
    c.begin("jpeg");
    // Within rounding of libjpeg-turbo: the inverse DCT, upsampling and colour conversion
    // are its own integer arithmetic.
    const Case full[] = {CASE(b444), CASE(b422), CASE(b420), CASE(gray), CASE(p420), CASE(p444), CASE(pgray), CASE(rst), CASE(exif6)};
    for (const Case& k : full) {
        JpegImage img;
        KV_CHECK(c, decode(k, 1, img));
        Diff d = compare(img, k.rgb, k.w, k.h);
        KV_INFO("test", "jpeg %-6s %ux%u: max %u, mean %.3f from Pillow's%s", k.name, img.width, img.height, d.max, d.mean, img.partial ? " (partial)" : "");
        KV_CHECK(c, d.max <= 2 && d.mean < 0.1 && !img.partial);
        release(img);
    }
    const Case scaled[] = {SCALED(b444, 2), SCALED(b444, 4), SCALED(b444, 8), SCALED(b420, 2), SCALED(b420, 4), SCALED(b420, 8),
                           SCALED(gray, 2), SCALED(gray, 4), SCALED(gray, 8), SCALED(p420, 2), SCALED(p420, 4), SCALED(p420, 8)};
    for (const Case& k : scaled) {
        u32 s = (u32)atoi(strchr(k.name, '/') + 1);
        JpegImage img;
        KV_CHECK(c, decode(k, s, img));
        Diff d = compare(img, k.rgb, k.w, k.h);
        KV_INFO("test", "jpeg %-8s %ux%u: max %u, mean %.3f from Pillow's", k.name, img.width, img.height, d.max, d.mean);
        KV_CHECK(c, d.max <= 2 && d.mean < 0.1);
        release(img);
    }
    // CMYK: recognisable, not colour-managed (Pillow's own conversion is a different
    // formula), so only near.
    {
        Case k = CASE(cmyk);
        JpegImage img;
        KV_CHECK(c, decode(k, 1, img));
        Diff d = compare(img, k.rgb, k.w, k.h);
        KV_INFO("test", "jpeg cmyk   %ux%u: max %u, mean %.3f from Pillow's", img.width, img.height, d.max, d.mean);
        KV_CHECK(c, d.mean < 24.0);
        release(img);
    }

    // The header alone.
    {
        JpegInfo info;
        const char* why = nullptr;
        KV_CHECK(c, jpeg_info(k_jpeg_p420, sizeof k_jpeg_p420, &info, &why) && info.width == 45 && info.height == 33 && info.components == 3 && info.progressive);
        KV_CHECK(c, jpeg_info(k_jpeg_gray, sizeof k_jpeg_gray, &info, &why) && info.components == 1 && !info.progressive && info.orientation == 1);
        KV_CHECK(c, jpeg_info(k_jpeg_exif6, sizeof k_jpeg_exif6, &info, &why) && info.orientation == 6 && info.width == 45);
        KV_CHECK(c, jpeg_info(k_jpeg_cmyk, sizeof k_jpeg_cmyk, &info, &why) && info.components == 4);
    }
    // Orientation reported, not applied.
    {
        Case k = CASE(exif6);
        JpegImage img;
        KV_CHECK(c, decode(k, 1, img, false) && img.width == 45 && img.height == 33 && img.orientation == 6);
        release(img);
        KV_CHECK(c, decode(k, 1, img, true) && img.width == 33 && img.height == 45 && img.orientation == 1);
        release(img);
    }

    // Truncated: a sequential file gives the rows it had, as they would have been; a
    // progressive one all its rows, from the scans it had.
    {
        Case k = CASE(b420);
        JpegImage whole, part;
        KV_CHECK(c, decode(k, 1, whole));
        JpegOptions o;
        const char* why = nullptr;
        KV_CHECK(c, jpeg_decode(k_jpeg_b420, sizeof k_jpeg_b420 * 85 / 100, o, nullptr, part, &why) && part.partial); // the scan two rows of MCUs in
        KV_CHECK(c, part.height > 0 && part.height < whole.height && part.height % 16 == 0 && part.width == whole.width);
        u32 same_rows = part.height > 16 ? part.height - 16 : 0; // the last MCU row may hold what was cut
        KV_CHECK(c, part.rgba && whole.rgba && memcmp(part.rgba, whole.rgba, (usize)same_rows * whole.width * 4) == 0);
        release(whole);
        release(part);
        JpegImage pp;
        KV_CHECK(c, jpeg_decode(k_jpeg_p420, sizeof k_jpeg_p420 * 7 / 10, o, nullptr, pp, &why) && pp.partial && pp.width == 45 && pp.height == 33);
        release(pp);
        KV_CHECK(c, !jpeg_decode(k_jpeg_b420, 300, o, nullptr, pp, &why) && !pp.rgba); // before the first row
    }
    // Refused, with a reason, nothing allocated.
    {
        JpegImage img;
        JpegOptions o;
        const char* why = nullptr;
        o.max_pixels = 45 * 33 - 1;
        KV_CHECK(c, !jpeg_decode(k_jpeg_b444, sizeof k_jpeg_b444, o, nullptr, img, &why) && !strcmp(why, "too large") && !img.rgba);
        o = JpegOptions{};
        o.scale = 3;
        KV_CHECK(c, !jpeg_decode(k_jpeg_b444, sizeof k_jpeg_b444, o, nullptr, img, &why));
        o = JpegOptions{};
        const unsigned char png[] = {137, 80, 78, 71, 13, 10, 26, 10};
        KV_CHECK(c, !jpeg_decode(png, sizeof png, o, nullptr, img, &why) && !strcmp(why, "not a JPEG"));
        // An arithmetic-coded frame: SOF0 made SOF9.
        static unsigned char arith[sizeof k_jpeg_b444];
        memcpy(arith, k_jpeg_b444, sizeof arith);
        for (usize i = 0; i + 1 < sizeof arith; ++i) if (arith[i] == 0xFF && arith[i + 1] == 0xC0) { arith[i + 1] = 0xC9; break; }
        KV_CHECK(c, !jpeg_decode(arith, sizeof arith, o, nullptr, img, &why) && strstr(why, "arithmetic"));
    }

    // A mutation fuzzer: each file with bytes changed, inserted and cut, many times over.
    // Nothing may read past the data or crash (run it under --sanitize address,undefined);
    // each decode either fails with a reason or gives an image of the size it says.
    {
        const Case all[] = {CASE(b444), CASE(b420), CASE(p420), CASE(pgray), CASE(rst), CASE(cmyk), CASE(exif6)};
        u32 seed = 0x1234567u, decoded = 0, refused = 0;
        auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
        static unsigned char buf[4096];
        for (const Case& k : all) {
            for (u32 round = 0; round < 400; ++round) {
                usize n = k.size < sizeof buf ? k.size : sizeof buf;
                memcpy(buf, k.data, n);
                u32 edits = 1 + rnd() % 6;
                for (u32 e = 0; e < edits; ++e) {
                    u32 what = rnd() % 4, at = (u32)(rnd() % n);
                    if (what == 0) buf[at] = (unsigned char)rnd();
                    else if (what == 1) buf[at] ^= (unsigned char)(1u << (rnd() % 8));
                    else if (what == 2 && n + 1 < sizeof buf) { memmove(buf + at + 1, buf + at, n - at); buf[at] = (unsigned char)rnd(); ++n; }
                    else n = at > 2 ? at : n;
                }
                JpegOptions o;
                o.scale = 1u << (rnd() % 4);
                o.max_pixels = 1u << 20;
                JpegImage img;
                const char* why = nullptr;
                if (jpeg_decode(buf, n, o, nullptr, img, &why)) {
                    ++decoded;
                    u64 sum = 0;
                    for (u32 i = 0; i < img.width * img.height * 4; i += 61) sum += img.rgba[i]; // every byte it gave is there to read
                    (void)sum;
                    release(img);
                } else {
                    ++refused;
                    KV_CHECK(c, why && why[0] && !img.rgba);
                }
            }
        }
        KV_INFO("test", "jpeg fuzz: %u mutated files, %u decoded, %u refused", decoded + refused, decoded, refused);
        KV_CHECK(c, decoded > 0 && refused > 0);
    }

    // A real cover, timed, when one is named.
    if (const char* path = getenv("KV_JPEG_BENCH")) {
        FILE* fp = fopen(path, "rb");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long sz = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            u8* data = (u8*)mem_alloc((usize)sz);
            if (fread(data, 1, (usize)sz, fp) == (usize)sz) {
                const u32 scales[] = {1u, 4u, 8u};
                for (u32 s : scales) {
                    f64 best = 1e9;
                    JpegImage img;
                    for (u32 i = 0; i < 5; ++i) {
                        JpegOptions o;
                        o.scale = s;
                        o.max_pixels = 1ull << 30;
                        const char* why = nullptr;
                        f64 t0 = time_now();
                        bool ok = jpeg_decode(data, (usize)sz, o, nullptr, img, &why);
                        f64 ms = (time_now() - t0) * 1000.0;
                        if (ms < best) best = ms;
                        if (ok && i < 4) release(img);
                    }
                    KV_INFO("test", "jpeg bench %s at 1/%u: %ux%u in %.2f ms (best of 5)", path, s, img.width, img.height, best);
                    release(img);
                }
            }
            mem_free(data);
            fclose(fp);
        }
    }
}
