#include "korvid/asset/png.h"
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
