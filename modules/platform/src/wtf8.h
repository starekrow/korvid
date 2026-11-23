// Korvid platform: WTF-8, the UTF-8 of a UTF-16 string that may hold half a surrogate
// pair. A Windows file name is UTF-16 that the file system does not check, so a name can
// hold a lone surrogate that UTF-8 has no form for. WTF-8 encodes it as UTF-8 would
// encode the code unit (ED A0 80 to ED BF BF), and decodes that back, so such a name
// lists, goes through the engine as a string and opens the same file. Every valid UTF-16
// string's WTF-8 is its UTF-8. win32/wide.h uses these; nothing here is Windows', so the
// tests run them everywhere.
#pragma once
#include "korvid/core/types.h"

namespace kv {

// UTF-16 (NUL-terminated) as WTF-8 into out, terminated. The bytes written, the NUL
// included, or 0 if they do not fit in cap. With out null, the room it needs.
inline int wtf8_from_utf16(const u16* w, char* out, int cap) {
    int n = 0;
    auto put = [&](u8 b) { if (out) { if (n >= cap) return false; out[n] = (char)b; } ++n; return true; };
    for (usize i = 0; w[i]; ++i) {
        u32 cp = w[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (w[i + 1] - 0xDC00);
            ++i;
        }
        bool ok;
        if (cp < 0x80) ok = put((u8)cp);
        else if (cp < 0x800) ok = put((u8)(0xC0 | (cp >> 6))) && put((u8)(0x80 | (cp & 0x3F)));
        else if (cp < 0x10000) ok = put((u8)(0xE0 | (cp >> 12))) && put((u8)(0x80 | ((cp >> 6) & 0x3F))) && put((u8)(0x80 | (cp & 0x3F)));
        else ok = put((u8)(0xF0 | (cp >> 18))) && put((u8)(0x80 | ((cp >> 12) & 0x3F))) && put((u8)(0x80 | ((cp >> 6) & 0x3F))) && put((u8)(0x80 | (cp & 0x3F)));
        if (!ok) return 0;
    }
    return put(0) ? n : 0;
}

// WTF-8 (NUL-terminated) as UTF-16 into out, terminated. The code units written, the
// NUL included; 0 if they do not fit in cap, -1 if s is not WTF-8 (a stray or missing
// continuation byte, an overlong form, a value past U+10FFFF). With out null, the room
// it needs. UTF-8 is WTF-8, so this is also the decoder for every valid name.
inline int utf16_from_wtf8(const char* s, u16* out, int cap) {
    const u8* p = (const u8*)s;
    int n = 0;
    bool full = false;
    auto put = [&](u32 u) { if (out) { if (n >= cap) { full = true; return; } out[n] = (u16)u; } ++n; };
    while (*p) {
        u8 b = *p;
        u32 cp, more, min;
        if (b < 0x80)            { cp = b;        more = 0; min = 0; }
        else if ((b >> 5) == 6)  { cp = b & 0x1F; more = 1; min = 0x80; }
        else if ((b >> 4) == 14) { cp = b & 0x0F; more = 2; min = 0x800; }
        else if ((b >> 3) == 30) { cp = b & 0x07; more = 3; min = 0x10000; }
        else return -1;
        ++p;
        for (u32 k = 0; k < more; ++k, ++p) {
            if ((*p & 0xC0) != 0x80) return -1;
            cp = (cp << 6) | (*p & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF) return -1;
        if (cp >= 0x10000) { put(0xD800 + ((cp - 0x10000) >> 10)); put(0xDC00 + ((cp - 0x10000) & 0x3FF)); }
        else put(cp);
    }
    put(0);
    return full ? 0 : n;
}

} // namespace kv
