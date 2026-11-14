#include "korvid/core/glob.h"
#include <string.h>

namespace kv {

namespace {

// One class at p (just past '['). Sets *end past the ']' and returns whether c is in it;
// *end is null when the class never closes.
bool class_match(const char* p, char c, const char** end) {
    bool negate = *p == '!' || *p == '^';
    if (negate) ++p;
    bool hit = false, first = true;
    for (; *p; first = false) {
        if (*p == ']' && !first) { *end = p + 1; return hit != negate; }
        char lo = *p++;
        if (lo == '\\' && *p) lo = *p++;
        char hi = lo;
        if (p[0] == '-' && p[1] && p[1] != ']') {
            hi = p[1];
            p += 2;
            if (hi == '\\' && *p) hi = *p++;
        }
        if ((u8)c >= (u8)lo && (u8)c <= (u8)hi) hit = true;
    }
    *end = nullptr;
    return false;
}

// One segment's pattern [p, pe) against one segment's text [s, se). Iterative, with the
// usual single backtrack point for the last star.
bool segment_match(const char* p, const char* pe, const char* s, const char* se) {
    const char* star_p = nullptr;
    const char* star_s = nullptr;
    while (s < se) {
        if (p < pe) {
            if (*p == '*') { star_p = ++p; star_s = s; continue; }
            if (*p == '?') { ++p; ++s; continue; }
            if (*p == '[') {
                const char* end;
                bool in = class_match(p + 1, *s, &end);
                if (!end || end > pe) return false;
                if (in) { p = end; ++s; continue; }
            } else {
                const char* q = p;
                if (*q == '\\' && q + 1 < pe) ++q;
                if (*q == *s) { p = q + 1; ++s; continue; }
            }
        }
        if (!star_p) return false;
        p = star_p;
        s = ++star_s;
    }
    while (p < pe && *p == '*') ++p;
    return p == pe;
}

// The end of the segment starting at p: the next unescaped slash, or the end. A slash
// inside a class does not occur in a pattern that means anything, so it is not looked for.
const char* segment_end(const char* p, bool pattern) {
    for (; *p; ++p) {
        if (pattern && *p == '\\' && p[1]) { ++p; continue; }
        if (*p == '/') break;
    }
    return p;
}

bool match_from(const char* p, const char* s, u32 depth) {
    if (depth > 64) return false;
    for (;;) {
        const char* pe = segment_end(p, true);
        if (pe - p == 2 && p[0] == '*' && p[1] == '*') {
            if (!*pe) return *s != 0;     // a trailing ** takes whatever is left, and needs something
            p = pe + 1;
            // ** followed by more: try the rest at this segment and at every later one.
            for (;;) {
                if (match_from(p, s, depth + 1)) return true;
                const char* se = segment_end(s, false);
                if (!*se) return false;
                s = se + 1;
            }
        }
        const char* se = segment_end(s, false);
        if (!segment_match(p, pe, s, se)) return false;
        if (!*pe || !*se) return !*pe && !*se;
        p = pe + 1;
        s = se + 1;
    }
}

} // namespace

bool glob_match(const char* pattern, const char* path) {
    if (!pattern || !path) return false;
    return match_from(pattern, path, 0);
}

bool glob_match_path(const char* pattern, const char* path) {
    if (!pattern || !path) return false;
    bool slash = false;
    for (const char* p = pattern; *p; ++p) {
        if (*p == '\\' && p[1]) { ++p; continue; }
        if (*p == '/') { slash = true; break; }
    }
    if (slash) return match_from(pattern, path, 0);
    const char* base = strrchr(path, '/');
    return match_from(pattern, base ? base + 1 : path, 0);
}

bool glob_is_literal(const char* pattern) {
    for (const char* p = pattern; p && *p; ++p)
        if (*p == '*' || *p == '?' || *p == '[' || *p == '\\') return false;
    return true;
}

} // namespace kv
