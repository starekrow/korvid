#include "korvid/image/png.h"
#include "korvid/core/test.h"
#include "png_data.h"
#include <string.h>

void test_png(kv::TestCtx& c) {
    using namespace kv;
    c.begin("png");

    // Every filter type on a row, decoded to the pixels the generator started from.
    PngImage img;
    const char* why = nullptr;
    KV_CHECK(c, png_decode(k_png_filters, sizeof k_png_filters, nullptr, img, &why));
    KV_CHECK(c, img.width == k_png_filters_w && img.height == k_png_filters_h);
    KV_CHECK(c, img.rgba && memcmp(img.rgba, k_png_filters_rgba, sizeof k_png_filters_rgba) == 0);
    mem_free(img.rgba);

    // Samples of fewer than 8 bits, packed many to a byte, rows not a whole byte long: a
    // palette (with a transparent entry) and gray.
    PngImage pal;
    KV_CHECK(c, png_decode(k_png_pal4, sizeof k_png_pal4, nullptr, pal, &why));
    KV_CHECK(c, pal.width == k_png_pal4_w && pal.height == k_png_pal4_h);
    KV_CHECK(c, pal.rgba && memcmp(pal.rgba, k_png_pal4_rgba, sizeof k_png_pal4_rgba) == 0);
    mem_free(pal.rgba);
    PngImage gray;
    KV_CHECK(c, png_decode(k_png_gray2, sizeof k_png_gray2, nullptr, gray, &why));
    KV_CHECK(c, gray.width == k_png_gray2_w && gray.height == k_png_gray2_h);
    KV_CHECK(c, gray.rgba && memcmp(gray.rgba, k_png_gray2_rgba, sizeof k_png_gray2_rgba) == 0);
    mem_free(gray.rgba);

    // Rejections name their reason and allocate nothing.
    MemStats before = mem_stats();
    PngImage bad;
    KV_CHECK(c, !png_decode("not a png at all, really", 24, nullptr, bad, &why) && strcmp(why, "not a PNG") == 0);
    KV_CHECK(c, !png_decode(k_png_filters, 40, nullptr, bad, &why) && why != nullptr); // truncated
    unsigned char corrupt[sizeof k_png_filters];
    memcpy(corrupt, k_png_filters, sizeof corrupt);
    corrupt[60] ^= 0xFF; // inside the IDAT stream
    KV_CHECK(c, !png_decode(corrupt, sizeof corrupt, nullptr, bad, &why) && strcmp(why, "corrupt image data") == 0);
    KV_CHECK(c, mem_stats().live_allocs == before.live_allocs);
}

// Encoding: what png_encode writes, png_decode reads back exactly, RGB and RGBA, at sizes
// that make the filters' edges count (one pixel, a row narrower than the byte offset,
// odd widths), over smooth gradients (where Sub, Up and Paeth win) and noise (None).
void test_png_encode(kv::TestCtx& c) {
    using namespace kv;
    c.begin("png encode");
    const u32 sizes[][2] = {{1, 1}, {1, 9}, {7, 5}, {33, 17}, {256, 64}};
    u32 seed = 12345;
    for (u32 channels = 3; channels <= 4; ++channels) {
        for (const auto& sz : sizes) {
            u32 w = sz[0], h = sz[1];
            usize n = (usize)w * h * channels;
            u8* src = (u8*)mem_general()->alloc(mem_general(), n, 8);
            for (u32 y = 0; y < h; ++y)
                for (u32 x = 0; x < w; ++x)
                    for (u32 k = 0; k < channels; ++k) {
                        seed = seed * 1103515245u + 12345u;
                        bool noisy = x > w / 2;
                        src[((usize)y * w + x) * channels + k] = noisy ? (u8)(seed >> 16) : (u8)(x * 3 + y * 5 + k * 40);
                    }
            Array<u8> file;
            const char* why = nullptr;
            KV_CHECK(c, png_encode(src, w, h, channels, file, &why));
            PngImage back;
            KV_CHECK(c, png_decode(file.data, file.len, nullptr, back, &why));
            bool same = back.rgba && back.width == w && back.height == h;
            for (usize i = 0; same && i < (usize)w * h; ++i)
                for (u32 k = 0; same && k < 4; ++k)
                    same = back.rgba[i * 4 + k] == (k < channels ? src[i * channels + k] : 255);
            KV_CHECK(c, same);
            mem_free(back.rgba);
            file.free();
            mem_general()->free(mem_general(), src, n);
        }
    }
    // A flat picture compresses to almost nothing; what cannot be written says so and
    // leaves the output alone.
    u8 flat[64 * 64 * 3];
    memset(flat, 90, sizeof flat);
    Array<u8> small;
    const char* why = nullptr;
    KV_CHECK(c, png_encode(flat, 64, 64, 3, small, &why) && small.len < 200);
    u32 before = small.len;
    KV_CHECK(c, !png_encode(flat, 64, 64, 2, small, &why) && small.len == before);
    KV_CHECK(c, !png_encode(flat, 0, 64, 3, small, &why) && small.len == before);
    small.free();
}
