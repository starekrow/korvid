// Korvid core: base64, RFC 4648's. Encoding writes the standard alphabet with padding,
// which is what JSON gets for a KBOR byte string (docs/design/kbor.md). Decoding takes
// the standard alphabet or the URL-safe one, padded or not: the two differ only in
// characters the other lacks, so which one a text is in is never in doubt, and a text
// that mixes them is refused.
#pragma once
#include "korvid/core/types.h"

namespace kv {

// The length of `n` bytes encoded, padding included (no NUL).
inline usize base64_encoded_len(usize n) { return (n + 2) / 3 * 4; }
// Writes base64_encoded_len(n) characters to `out` (no NUL).
void base64_encode(const void* data, usize n, char* out);

// The most bytes `n` characters can decode to.
inline usize base64_decoded_max(usize n) { return (n + 3) / 4 * 3; }
// Decodes `n` characters into `out` (base64_decoded_max(n) bytes of room) and sets *len.
// False for anything that is not base64: a character of neither alphabet, both alphabets
// in one text, padding anywhere but the end or of the wrong length, a lone last
// character, or bits left over in the last one.
bool base64_decode(const char* text, usize n, u8* out, usize* len);

} // namespace kv
