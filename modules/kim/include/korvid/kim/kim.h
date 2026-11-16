// kim: the .kim parser and compiler (docs/PRDs/Krome-Slice-2-PRD.md). Text in; the
// document out as a JsonDoc in the shape a .kimb holds, or as the .kimb's bytes: KBOR,
// normalized (core's kbor_write), canonical, so the same document always gives the same
// bytes whatever its comments, spacing or the order of a line's properties.
//
// The grammar (one node per line; a block in braces):
//
//   use "krome/std"                      a prelude, by asset path
//   script "game/form.lua"               a Lua file with the screen's handlers
//   prop size number                     a typed parameter for definitions
//   style button [base] { bg=panel2 pad=8,4  hover { bg=panel3 } }
//   theme dark { panel2=#202430 accent=#2f6fed }
//   def field label value { row { text "{label}" slot } }
//   screen form { col [page] gap=8 { button "Save" -> save } }
//
// A node: a word; then in any order flags (bare words), properties (name=value), content
// strings, a bare range (1..99), a binding (@name), an event (-> name) and styles ([a b]);
// then an optional block of children. `each item in list { }` and `if name { }` are nodes
// too. `//` comments to the end of a line wherever white space may stand.
//
// Values: numbers (8, -2, 1.5), percentages (40%, kept as the string "40%"), a value bound
// to the model (@name, kept as "@name"), strings ("..."), colors (#rrggbb, #rrggbbaa, #rgb, #rgba), ranges
// (120..400, ..400, 120..), lists (4,8 with no spaces) and bare words (a token or a
// keyword: Krome decides which).
//
// The document:
//
//   { "kim": 1, "file": F, "uses": [..], "scripts": [..], "props": [{"name","type"}..],
//     "styles": {NAME: {"s": [..], "f": [..], "p": {..}, "looks": {STATE: {..}}}},
//     "themes": {NAME: {TOKEN: value}}, "defs": {NAME: {"params": [..], "body": node}},
//     "screens": {NAME: node}, "lines": [..] }
//
//   node: { "w", "id", "f": [flags], "p": {props}, "s": [styles], "t": text, "a": [more
//           strings], "r": [min, max] (null for an open end), "b": binding, "bv": the value of
//           @key=value, "e": event,
//           "v" and "in" (each), "on" (if), "c": [children] }, each key only when present.
//
// Canonical: maps in name order (properties, styles, themes, definitions, screens, a
// style's looks, a theme's tokens); a repeated property or token takes its last value;
// flags, styles, strings, uses and scripts in the order written; empty sections left out.
//
// Lines are a sidecar, "lines", one per entry in this walk: each use, each script, each
// prop; each screen by name (its own line, then its nodes depth first); each definition by
// name (its line, then its body's nodes); each style by name (its line, then each look);
// each theme by name (its line, then each token). Leaving it out (KV_KIM_NO_LINES)
// changes no other byte.
#pragma once
#include "korvid/core/array.h"
#include "korvid/core/json.h"
#include "korvid/kimb/kimb.h"

namespace kv {

constexpr u32 KV_KIM_VERSION    = 1;
constexpr u32 KV_KIM_MAX_ERRORS = KV_KIMB_MAX_ERRORS;
constexpr u32 KV_KIM_NO_LINES   = 1u << 0;

// Errors are kimb's (KimError, KimErrors: korvid/kimb/kimb.h), so a mistake reads the
// same from the parser as from the check.

// Parses .kim text into the document. Nodes and strings from `alloc` (an arena: nothing
// here frees). `file` is the asset path the document records. False, and the document a
// lone null, when there was any error; every error found is in `errors`.
bool kim_parse(const char* text, usize len, const char* file, Allocator* alloc, JsonDoc& doc, KimErrors& errors, u32 flags = 0);

// kim_parse, then kbor_write: the .kimb's bytes appended to `out`. Its working memory is
// its own and freed before it returns.
bool kim_compile(const char* text, usize len, const char* file, Array<u8>& out, KimErrors& errors, u32 flags = 0);

} // namespace kv
