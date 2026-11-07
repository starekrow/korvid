#include "korvid/platform/input.h"
#include "korvid/core/test.h"
#include <string.h>

void test_keymap(kv::TestCtx& c) {
    using namespace kv;
    c.begin("keymap");

    // Every key except Unknown round-trips through the scancode table.
    for (u32 i = 1; i < (u32)Key::Count; ++i) {
        Key k  = (Key)i;
        u32 sc = scancode_from_key(k);
        KV_CHECK(c, sc != 0);
        KV_CHECK(c, key_from_scancode(sc) == k);
        KV_CHECK(c, strcmp(key_name(k), "Unknown") != 0 && strcmp(key_name(k), "Invalid") != 0);
    }
    KV_CHECK(c, key_from_scancode(0) == Key::Unknown);
    KV_CHECK(c, key_from_scancode(0xFFFFFFFFu) == Key::Unknown);
    KV_CHECK(c, scancode_from_key(Key::Unknown) == 0);
    KV_CHECK(c, strcmp(key_name(Key::Count), "Invalid") == 0);

    // InputState bit set.
    InputState s;
    KV_CHECK(c, !s.down(Key::A));
    s.set(Key::A, true);
    s.set(Key::RAlt, true);
    KV_CHECK(c, s.down(Key::A) && s.down(Key::RAlt) && !s.down(Key::B));
    s.set(Key::A, false);
    KV_CHECK(c, !s.down(Key::A) && s.down(Key::RAlt));
    s.clear();
    KV_CHECK(c, !s.down(Key::RAlt));

    // Sampling copies raw once; later raw changes do not leak into the sample.
    Input in;
    in.raw.set(Key::Left, true);
    in.sample();
    in.raw.set(Key::Left, false);
    KV_CHECK(c, in.sampled.down(Key::Left) && !in.raw.down(Key::Left));

    // Pads ride along: buttons as bits, axes as floats, the first connected one is pad().
    KV_CHECK(c, in.raw.pad() == nullptr);
    in.raw.pads[1].connected = true;
    in.raw.pads[1].set(PadButton::South, true);
    in.raw.pads[1].axes[(u32)PadAxis::LeftX] = -0.75f;
    in.sample();
    KV_CHECK(c, in.sampled.pad() == &in.sampled.pads[1] && in.sampled.pad()->down(PadButton::South) && !in.sampled.pad()->down(PadButton::East));
    KV_CHECK(c, in.sampled.pad()->axis(PadAxis::LeftX) == -0.75f);
    in.raw.clear();
    KV_CHECK(c, in.raw.pads[1].connected && !in.raw.pads[1].down(PadButton::South) && in.raw.pads[1].axis(PadAxis::LeftX) == 0.0f);
    KV_CHECK(c, strcmp(pad_button_name(PadButton::DpadRight), "DpadRight") == 0 && strcmp(pad_axis_name(PadAxis::Count), "Invalid") == 0);
}
