// PPM: a round trip, a header with comments and spacing, and every way to refuse.
#include "korvid/image/ppm.h"
#include "korvid/core/test.h"
#include <string.h>

void test_ppm(kv::TestCtx& c) {
    using namespace kv;
    c.begin("ppm");
    u8 rgb[5 * 3 * 3];
    for (u32 i = 0; i < sizeof rgb; ++i) rgb[i] = (u8)(i * 7);
    Array<u8> file;
    ppm_encode(rgb, 5, 3, file);
    PpmImage img;
    const char* why = nullptr;
    KV_CHECK(c, ppm_decode(file.data, file.len, nullptr, img, &why));
    KV_CHECK(c, img.width == 5 && img.height == 3 && img.rgb && memcmp(img.rgb, rgb, sizeof rgb) == 0);
    mem_free(img.rgb);

    // Comments and odd spacing in the header; the pixels start after one whitespace byte.
    const char head[] = "P6 # made by hand\n2\t# width\n 1\n255\n";
    u8 commented[sizeof head - 1 + 6];
    memcpy(commented, head, sizeof head - 1);
    const u8 px[6] = {10, 20, 30, 40, 50, 60};
    memcpy(commented + sizeof head - 1, px, 6);
    PpmImage two;
    KV_CHECK(c, ppm_decode(commented, sizeof commented, nullptr, two, &why) && two.width == 2 && two.height == 1 && memcmp(two.rgb, px, 6) == 0);
    mem_free(two.rgb);

    // Refusals allocate nothing and say why.
    MemStats before = mem_stats();
    PpmImage bad;
    KV_CHECK(c, !ppm_decode("P3\n1 1\n255\n", 11, nullptr, bad, &why) && strcmp(why, "not a binary PPM") == 0);
    KV_CHECK(c, !ppm_decode("P6\n1 1\n65535\n", 13, nullptr, bad, &why) && strcmp(why, "not an 8-bit PPM") == 0);
    KV_CHECK(c, !ppm_decode(file.data, file.len - 1, nullptr, bad, &why) && strcmp(why, "truncated") == 0);
    KV_CHECK(c, !ppm_decode("P6\n0 1\n255\n", 11, nullptr, bad, &why));
    KV_CHECK(c, !ppm_decode("P6\nx 1\n255\n", 11, nullptr, bad, &why) && strcmp(why, "bad PPM header") == 0);
    KV_CHECK(c, mem_stats().live_allocs == before.live_allocs);
    file.free();
}
