#include "korvid/core/utf8.h"
#include "korvid/core/test.h"
#include <string.h>

void test_utf8(kv::TestCtx& c) {
    using namespace kv;
    c.begin("utf8");

    // Every length, and the boundaries of each.
    struct Case { const char* bytes; u32 cp; u32 len; };
    const Case good[] = {
        {"A", 0x41, 1}, {"\x7f", 0x7F, 1}, {"\xc2\x80", 0x80, 2}, {"\xc3\xa9", 0xE9, 2}, {"\xdf\xbf", 0x7FF, 2},
        {"\xe0\xa0\x80", 0x800, 3}, {"\xe2\x80\x94", 0x2014, 3}, {"\xed\x9f\xbf", 0xD7FF, 3}, {"\xee\x80\x80", 0xE000, 3},
        {"\xef\xbf\xbd", 0xFFFD, 3}, {"\xef\xbf\xbf", 0xFFFF, 3}, {"\xf0\x90\x80\x80", 0x10000, 4},
        {"\xf0\x9f\x98\x80", 0x1F600, 4}, {"\xf4\x8f\xbf\xbf", 0x10FFFF, 4},
    };
    for (const Case& k : good) {
        u32 adv = 0;
        KV_CHECK(c, utf8_decode(k.bytes, strlen(k.bytes), &adv) == k.cp && adv == k.len);
        char enc[4];
        KV_CHECK(c, utf8_encode(k.cp, enc) == k.len && memcmp(enc, k.bytes, k.len) == 0);
        KV_CHECK(c, utf8_valid(k.bytes, k.len));
    }

    // Malformed: each decodes as U+FFFD and advances one byte.
    const char* bad[] = {
        "\x80", "\xbf", "\xc0\x80", "\xc1\xbf", "\xe0\x80\x80", "\xe0\x9f\xbf", "\xf0\x8f\xbf\xbf", // overlong
        "\xed\xa0\x80", "\xed\xbf\xbf",                                                        // surrogates
        "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\xff", "\xfe",                                // too large, never valid
        "\xc3", "\xe2\x80", "\xf0\x9f\x98",                                                   // truncated
        "\xc3\x41", "\xe2\x80\x41",                                                           // continuation missing
    };
    for (const char* b : bad) {
        u32 adv = 0;
        KV_CHECK(c, utf8_decode(b, strlen(b), &adv) == KV_UTF8_REPLACEMENT && adv == 1);
        KV_CHECK(c, !utf8_valid(b, strlen(b)));
    }
    u32 adv = 9;
    KV_CHECK(c, utf8_decode("x", 0, &adv) == 0 && adv == 0);
    // A length bound shorter than the sequence is a truncation, not a read past it.
    KV_CHECK(c, utf8_decode("\xc3\xa9", 1, &adv) == KV_UTF8_REPLACEMENT && adv == 1);

    // utf8_next walks a string, stops at the NUL, and makes progress through garbage.
    const char* s = "a\xc3\xa9\xff\xe2\x80\x94z";
    const char* p = s;
    u32 seen[8], n = 0;
    while (u32 cp = utf8_next(p)) seen[n++] = cp;
    KV_CHECK(c, n == 5 && seen[0] == 'a' && seen[1] == 0xE9 && seen[2] == 0xFFFD && seen[3] == 0x2014 && seen[4] == 'z');
    KV_CHECK(c, *p == 0 && utf8_next(p) == 0 && *p == 0);
    const char* cut = "\xe2\x80"; // cut short by the NUL: two replacements, never past the end
    p = cut;
    KV_CHECK(c, utf8_next(p) == KV_UTF8_REPLACEMENT && utf8_next(p) == KV_UTF8_REPLACEMENT && utf8_next(p) == 0 && p == cut + 2);
    KV_CHECK(c, utf8_count(s) == 5 && utf8_count("") == 0);

    // Encoding what cannot be encoded.
    char enc[4];
    KV_CHECK(c, utf8_encode(0xD800, enc) == 3 && memcmp(enc, "\xef\xbf\xbd", 3) == 0);
    KV_CHECK(c, utf8_encode(0x110000, enc) == 3 && memcmp(enc, "\xef\xbf\xbd", 3) == 0);

    // Truncation at a boundary: every cut of "é—😀" keeps only whole characters.
    const char* mixed = "\xc3\xa9\xe2\x80\x94\xf0\x9f\x98\x80"; // 2 + 3 + 4 bytes
    const usize keep[] = {0, 0, 2, 2, 2, 5, 5, 5, 5, 9};
    for (usize len = 0; len <= 9; ++len) KV_CHECK(c, utf8_prefix(mixed, len) == keep[len]);
    KV_CHECK(c, utf8_prefix("abc", 2) == 2);
    KV_CHECK(c, utf8_prefix("\x80\x80", 2) == 2); // malformed is kept as it is

    char buf[6];
    memset(buf, 'X', sizeof buf);
    KV_CHECK(c, utf8_copy(buf, sizeof buf, mixed) == 5 && memcmp(buf, "\xc3\xa9\xe2\x80\x94\0", 6) == 0);
    memset(buf, 'X', sizeof buf);
    KV_CHECK(c, utf8_copy(buf, sizeof buf, "ab") == 2 && buf[2] == 0 && buf[5] == 0); // the rest is zeroed
    KV_CHECK(c, utf8_copy(buf, sizeof buf, nullptr) == 0 && buf[0] == 0);
    KV_CHECK(c, utf8_copy(buf, 0, "ab") == 0);
    KV_CHECK(c, utf8_copy(buf, 1, "\xc3\xa9") == 0 && buf[0] == 0);

    char t[8] = "ab\xe2\x80";
    utf8_trim(t);
    KV_CHECK(c, strcmp(t, "ab") == 0);
    char u[8] = "ab\xc3\xa9";
    utf8_trim(u);
    KV_CHECK(c, strcmp(u, "ab\xc3\xa9") == 0);
}
