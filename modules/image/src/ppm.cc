#include "korvid/image/ppm.h"
#include <stdio.h>
#include <string.h>

namespace kv {

namespace {

bool ppm_fail(const char** error, const char* why) { if (error) *error = why; return false; }
bool is_space(u8 c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }

// The next header field: whitespace and comments skipped, then a run of digits (or the
// magic). False at the end of the data.
bool field(const u8* p, usize size, usize& at, usize& start, usize& end) {
    for (;;) {
        while (at < size && is_space(p[at])) ++at;
        if (at < size && p[at] == '#') { while (at < size && p[at] != '\n') ++at; continue; }
        break;
    }
    start = at;
    while (at < size && !is_space(p[at]) && p[at] != '#') ++at;
    end = at;
    return end > start;
}

bool number(const u8* p, usize start, usize end, u32& v) {
    if (end - start > 9) return false;
    v = 0;
    for (usize i = start; i < end; ++i) {
        if (p[i] < '0' || p[i] > '9') return false;
        v = v * 10 + (u32)(p[i] - '0');
    }
    return true;
}

} // namespace

bool ppm_decode(const void* data, usize size, Allocator* pixels, PpmImage& out, const char** error) {
    const u8* p = (const u8*)data;
    usize at = 0, s = 0, e = 0;
    if (!field(p, size, at, s, e) || e - s != 2 || p[s] != 'P' || p[s + 1] != '6') return ppm_fail(error, "not a binary PPM");
    u32 v[3];
    for (u32 i = 0; i < 3; ++i)
        if (!field(p, size, at, s, e) || !number(p, s, e, v[i])) return ppm_fail(error, "bad PPM header");
    if (v[2] != 255) return ppm_fail(error, "not an 8-bit PPM");
    if (v[0] == 0 || v[1] == 0 || v[0] > (1u << 16) || v[1] > (1u << 16)) return ppm_fail(error, "bad size");
    if (at >= size || !is_space(p[at])) return ppm_fail(error, "bad PPM header");
    ++at; // the one whitespace byte after maxval
    usize bytes = (usize)v[0] * v[1] * 3;
    if (size - at < bytes) return ppm_fail(error, "truncated");
    Allocator* a = pixels ? pixels : mem_general();
    u8* rgb = (u8*)alloc_in(a, bytes);
    if (!rgb) return ppm_fail(error, "out of memory");
    memcpy(rgb, p + at, bytes);
    out.width  = v[0];
    out.height = v[1];
    out.rgb    = rgb;
    return true;
}

void ppm_encode(const u8* rgb, u32 width, u32 height, Array<u8>& out) {
    char head[48];
    int n = snprintf(head, sizeof head, "P6\n%u %u\n255\n", width, height);
    usize bytes = (usize)width * height * 3;
    out.reserve(out.len + (u32)n + (u32)bytes);
    memcpy(out.push_n((u32)n), head, (usize)n);
    if (bytes) memcpy(out.push_n((u32)bytes), rgb, bytes);
}

} // namespace kv
