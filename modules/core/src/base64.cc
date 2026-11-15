#include "korvid/core/base64.h"

namespace kv {

namespace {
constexpr char k_standard[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

void base64_encode(const void* data, usize n, char* out) {
    const u8* p = (const u8*)data;
    usize i = 0;
    for (; i + 3 <= n; i += 3) {
        u32 v = (u32)p[i] << 16 | (u32)p[i + 1] << 8 | p[i + 2];
        *out++ = k_standard[v >> 18];
        *out++ = k_standard[(v >> 12) & 63];
        *out++ = k_standard[(v >> 6) & 63];
        *out++ = k_standard[v & 63];
    }
    if (i < n) {
        u32 v = (u32)p[i] << 16 | (i + 1 < n ? (u32)p[i + 1] << 8 : 0);
        *out++ = k_standard[v >> 18];
        *out++ = k_standard[(v >> 12) & 63];
        *out++ = i + 1 < n ? k_standard[(v >> 6) & 63] : '=';
        *out++ = '=';
    }
}

bool base64_decode(const char* text, usize n, u8* out, usize* len) {
    *len = 0;
    usize pad = 0;
    while (pad < 2 && n > pad && text[n - 1 - pad] == '=') ++pad;
    usize body = n - pad;
    if (pad && n % 4) return false;   // padding comes only to make whole groups
    if (body % 4 == 1) return false;  // one character is six bits, not a byte
    bool standard = false, url = false;
    u32 acc = 0, bits = 0;
    usize o = 0;
    for (usize i = 0; i < body; ++i) {
        char c = text[i];
        u32 d;
        if (c >= 'A' && c <= 'Z') d = (u32)(c - 'A');
        else if (c >= 'a' && c <= 'z') d = (u32)(c - 'a' + 26);
        else if (c >= '0' && c <= '9') d = (u32)(c - '0' + 52);
        else if (c == '+' || c == '/') { standard = true; d = c == '+' ? 62 : 63; }
        else if (c == '-' || c == '_') { url = true; d = c == '-' ? 62 : 63; }
        else return false;
        acc = acc << 6 | d;
        bits += 6;
        if (bits >= 8) { bits -= 8; out[o++] = (u8)(acc >> bits); acc &= (1u << bits) - 1; }
    }
    if (standard && url) return false;
    if (pad && pad != (4 - body % 4) % 4) return false;
    if (acc) return false; // the last character's spare bits are zero in an encoding
    *len = o;
    return true;
}

} // namespace kv
