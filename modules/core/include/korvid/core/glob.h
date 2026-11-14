// Korvid core: glob matching for paths. Forward slashes only; a pattern and a path are
// both relative and neither starts with a slash.
//
//   *        any run of characters within one segment (never a slash)
//   ?        one character that is not a slash
//   [abc]    one of these; [a-z] a range; [!abc] none of these; never a slash
//   **       as a whole segment: any number of segments, none included
//            ("a/**/b" matches "a/b"; "a/**" matches everything under a, not a itself;
//            "**/b" matches b at any depth)
//   \x       x, literally
//
// Matching is bytewise and case-sensitive. A malformed class ("[abc") matches nothing.
#pragma once
#include "korvid/core/types.h"

namespace kv {

// The whole of `path` against the whole of `pattern`.
bool glob_match(const char* pattern, const char* path);

// As a rule file means it: a pattern with no slash names a file at any depth
// ("*.png" matches "ships/big.png"); a pattern with one is matched from the top.
bool glob_match_path(const char* pattern, const char* path);

// True when the text has no glob characters: it can be compared, not matched.
bool glob_is_literal(const char* pattern);

} // namespace kv
