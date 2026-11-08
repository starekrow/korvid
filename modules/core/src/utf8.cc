#include "korvid/core/utf8.h"
#include <string.h>

namespace kv {

u32 utf8_decode(const char* s, usize len, u32* advance) {
    if (len == 0) { *advance = 0; return 0; }
    const u8* p = (const u8*)s;
    u8 b0 = p[0];
    *advance = 1;
    if (b0 < 0x80) return b0;
    u32 need, cp, min;
    if      (b0 >= 0xC2 && b0 <= 0xDF) { need = 1; cp = b0 & 0x1F; min = 0x80; }
    else if (b0 >= 0xE0 && b0 <= 0xEF) { need = 2; cp = b0 & 0x0F; min = 0x800; }
    else if (b0 >= 0xF0 && b0 <= 0xF4) { need = 3; cp = b0 & 0x07; min = 0x10000; }
    else return KV_UTF8_REPLACEMENT; // a continuation byte, C0/C1 (always overlong), or F5..FF
    if (len < 1 + need) return KV_UTF8_REPLACEMENT;
    for (u32 i = 1; i <= need; ++i) {
        u8 b = p[i];
        if ((b & 0xC0) != 0x80) return KV_UTF8_REPLACEMENT; // also stops at a NUL
        cp = (cp << 6) | (b & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return KV_UTF8_REPLACEMENT;
    *advance = 1 + need;
    return cp;
}

u32 utf8_next(const char*& p) {
    if (!*p) return 0;
    // At most four bytes are looked at, and a NUL stops the continuation check before
    // anything past it is read, so the length bound can be generous.
    u32 adv;
    u32 cp = utf8_decode(p, KV_UTF8_MAX_BYTES, &adv);
    p += adv;
    return cp;
}

u32 utf8_encode(u32 cp, char out[KV_UTF8_MAX_BYTES]) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = KV_UTF8_REPLACEMENT;
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

bool utf8_valid(const char* s, usize len) {
    usize at = 0;
    while (at < len) {
        u32 adv;
        u32 cp = utf8_decode(s + at, len - at, &adv);
        // U+FFFD itself is three bytes; a one-byte replacement is a malformed byte.
        if (cp == KV_UTF8_REPLACEMENT && adv == 1) return false;
        at += adv;
    }
    return true;
}

usize utf8_count(const char* s) {
    usize n = 0;
    while (utf8_next(s)) ++n;
    return n;
}

usize utf8_prefix(const char* s, usize len) {
    if (len == 0) return 0;
    // Walk back over at most three continuation bytes to the sequence's lead.
    usize lead = len;
    u32 back = 0;
    while (lead > 0 && back < 3 && ((u8)s[lead - 1] & 0xC0) == 0x80) { --lead; ++back; }
    if (lead == 0) return len;              // only continuation bytes: malformed, kept
    u8 b0 = (u8)s[lead - 1];
    u32 need = b0 >= 0xC2 && b0 <= 0xDF ? 1 : b0 >= 0xE0 && b0 <= 0xEF ? 2 : b0 >= 0xF0 && b0 <= 0xF4 ? 3 : 0;
    if (need == 0) return len;              // ASCII or a malformed lead: nothing is cut
    if (back < need) return lead - 1;       // the last sequence is cut: drop it
    return len;
}

usize utf8_copy(char* dst, usize cap, const char* src) {
    if (cap == 0) return 0;
    usize n = src ? strlen(src) : 0;
    if (n > cap - 1) n = utf8_prefix(src, cap - 1);
    if (n) memcpy(dst, src, n);
    memset(dst + n, 0, cap - n);
    return n;
}

void utf8_trim(char* s) {
    usize n = strlen(s);
    s[utf8_prefix(s, n)] = 0;
}

} // namespace kv
