// Korvid core: UTF-8, the engine's one text encoding. Strings everywhere (labels, paths,
// JSON, the CLI, char fields) are UTF-8 bytes, NUL-terminated unless a length is given.
//
// Two jobs bring code here. Drawing or measuring text walks it a code point at a time
// (utf8_next, utf8_decode), since a glyph is looked up by code point, not by byte. And
// text put into a fixed buffer (a char field in a component, a formatted label) is cut at
// a code point boundary (utf8_copy, utf8_trim), so it never ends on half a character.
//
// Decoding is total: a malformed sequence (a stray continuation byte, a truncated
// sequence, an overlong form, a surrogate, anything past U+10FFFF) decodes as U+FFFD and
// advances exactly one byte, so a loop over bad input always makes progress and never
// reads past the end. Nothing here allocates. Normalization, case and grapheme clusters
// are not here: two spellings of one character are two strings.
#pragma once
#include "korvid/core/types.h"

namespace kv {

constexpr u32 KV_UTF8_REPLACEMENT = 0xFFFD; // U+FFFD, what a malformed byte decodes as
constexpr u32 KV_UTF8_MAX_BYTES   = 4;      // the longest sequence, and utf8_encode's room

// Decodes the code point at `s`, reading at most `len` bytes, and stores how many bytes
// it took in *advance (at least 1 when len > 0; 0 when len == 0, and then returns 0). The
// form for text with a length (from modules/typeset/src/typeset.cc):
//
//   for (u32 at = 0, step; at < len; at += step) {
//       u32 cp = utf8_decode(text + at, len - at, &step);
//       ...
//   }
u32 utf8_decode(const char* s, usize len, u32* advance);

// The next code point of a NUL-terminated string, advancing `p` past it. At the NUL it
// returns 0 and leaves `p` on it. A sequence cut short by the NUL is malformed. The form
// for a NUL-terminated string (from modules/text/src/module.cc):
//
//   const char* p = text.str;
//   while (u32 cp = utf8_next(p)) {
//       const Glyph* g = font_glyph_or_replacement(font, cp);
//       ...
//   }
u32 utf8_next(const char*& p);

// Encodes `cp` into out (up to 4 bytes, no NUL) and returns the byte count. Surrogates
// and values past U+10FFFF encode as U+FFFD.
u32 utf8_encode(u32 cp, char out[KV_UTF8_MAX_BYTES]);

// True if every byte of [s, s + len) is part of a well-formed sequence.
bool utf8_valid(const char* s, usize len);

// Code points in a NUL-terminated string, each malformed byte counting as one. O(n).
usize utf8_count(const char* s);

// The longest prefix of [s, s + len) that does not end inside a sequence: what a
// truncation to `len` bytes should keep. Well-formed input only loses a cut sequence;
// malformed bytes are kept as they are (they decode as U+FFFD one at a time anyway).
usize utf8_prefix(const char* s, usize len);

// Copies the NUL-terminated `src` into dst[cap], truncating at a code point boundary so
// the copy is never a cut sequence, NUL-terminates, and zeroes the rest of dst so fixed
// fields hash the same whatever they held before. Returns the bytes copied, NUL excluded.
// cap 0 copies nothing; a null `src` copies "".
//
//   utf8_copy(e.path, sizeof e.path, where); // a long path is cut, never mid-character
usize utf8_copy(char* dst, usize cap, const char* src);

// Shortens a NUL-terminated string in place so it does not end in a cut sequence; for a
// buffer that something like vsnprintf may have truncated mid-character:
//
//   int n = vformat(t.str, sizeof t.str, fmt, args);
//   if (n >= (int)sizeof t.str) utf8_trim(t.str);
void utf8_trim(char* s);

} // namespace kv
