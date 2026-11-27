// kimb: the .kimb document, the scope its names resolve in, and the check (docs/PRDs/
// Krome-Slice-2-PRD.md, "Meaning"). The document's shape is kim's (korvid/kim/kim.h).
//
//   KimbFile f;
//   kimb_read(bytes, size, &arena.a, f, errors);         // KBOR, the version, the lines
//   KimbScope scope;
//   kimb_scope(scope, f, find_use, user, errors);         // f, then its uses, depth first
//   KimbFound d;
//   if (kimb_find_def(scope, "button", &d)) ...           // the first file in the scope that has it
//   kimb_check(scope, errors);                            // every mistake in f, with its line
//
// Names resolve against the file's own definitions, styles and themes, then each `use` in
// the order written (and theirs, depth first, each file once), then the kernel. A token
// is looked up in the theme of that name in the same order, so a game's `theme dark`
// overrides the tokens of std's that it names and keeps the rest.
#pragma once
#include "korvid/core/json.h"

namespace kv {

constexpr u32 KV_KIMB_MAX_SCOPE  = 32;
constexpr u32 KV_KIMB_MAX_ERRORS = 32;

// A mistake, with where it was made. `file` and `used_file` point at text that outlives
// the errors (a document's strings, a caller's names). A mistake found through a
// definition also names where it was used.
struct KimError {
    const char* file      = nullptr;
    u32         line      = 0;
    u32         col       = 0; // 1-based; 0 when only the line is known
    const char* used_file = nullptr;
    u32         used_line = 0;
    char        text[200] = {};
};

struct KimErrors {
    KimError list[KV_KIMB_MAX_ERRORS];
    u32      count = 0; // stored
    u32      total = 0; // found; past the cap they are counted, not kept

    KimError* add(const char* file, u32 line, u32 col, const char* fmt, ...) KV_PRINTF(5, 6);
    // "file:line:col: text", and " (used at file:line)" when there is one.
    i32 format_one(u32 i, char* out, usize cap) const;
};

// A .kimb read. Each node, definition, style, look, theme and token carries its line in
// JsonValue::line (0 when the file was stripped of them).
struct KimbFile {
    JsonDoc          doc;
    const char*      file    = "";      // the asset path it records ("game/form.kim")
    const JsonValue* uses    = nullptr; // each section, or null when absent
    const JsonValue* scripts = nullptr;
    const JsonValue* props   = nullptr;
    const JsonValue* screens = nullptr;
    const JsonValue* defs    = nullptr;
    const JsonValue* styles  = nullptr;
    const JsonValue* themes  = nullptr;
    const JsonValue* fonts   = nullptr; // bitmap fonts inline: {NAME: {image, cell, chars, missing, advance}}
    bool             lines   = false;
};

// Reads a .kimb into `f`: nodes and strings from `alloc`. False, with why in `errors`
// under `name`, for bytes that are not KBOR, a version this does not read, or a shape
// that is not a .kimb.
bool kimb_read(const void* data, usize size, Allocator* alloc, KimbFile& f, KimErrors& errors, const char* name);
// The same from a document already in memory (kim_parse's): its sections, and its lines
// moved from the sidecar onto the values.
bool kimb_from_doc(JsonDoc& doc, KimbFile& f, KimErrors& errors, const char* name);

// The files a name resolves in, in order: the file, then its uses.
struct KimbScope {
    const KimbFile* list[KV_KIMB_MAX_SCOPE] = {};
    u32             count = 0;
};

// The file a `use` names ("krome/std"), or null when it is not there (an error at the
// use's line).
using KimbFindUse = const KimbFile* (*)(void* user, const char* use);
bool kimb_scope(KimbScope& scope, const KimbFile& f, KimbFindUse find, void* user, KimErrors& errors);

struct KimbFound {
    const KimbFile*  file  = nullptr;
    const JsonValue* value = nullptr;
};
bool kimb_find_def(const KimbScope& s, const char* name, KimbFound* out);
bool kimb_find_style(const KimbScope& s, const char* name, KimbFound* out);
// A bitmap font written inline (`font digits { ... }`), by name, the first file's that has it.
bool kimb_find_font(const KimbScope& s, const char* name, KimbFound* out);
bool kimb_find_token(const KimbScope& s, const char* theme, const char* token, KimbFound* out);
bool kimb_find_prop(const KimbScope& s, const char* name, KimbFound* out); // a `prop` declaration: {name, type}
bool kimb_has_theme(const KimbScope& s, const char* theme);
// The theme a scope starts in: the first in resolution order (a file's by name); null
// when there is none.
const char* kimb_first_theme(const KimbScope& s);

// ---- the kernel's vocabulary ----------------------------------------------------------------

enum class KimbType : u8 {
    Number,  // 8, 1.5
    Color,   // #rrggbb, #rrggbbaa, #rgb, none
    String,  // an asset path
    Size,    // a number, hug, fill, or a range "120..400"
    Box4,    // one, two or four numbers (pad, radius)
    Shadow,  // three or four numbers
    Dir,     // row, col, stack
    Anchor,  // top-left ... bottom-right, center
    Any,     // a declared parameter of no type, or one of type "any"
};

struct KimbKernelProp {
    const char* name;
    KimbType    type;
    bool        layout; // a node's own, not a look's: a style's looks cannot set it
};

const KimbKernelProp* kimb_kernel_prop(const char* name);
bool kimb_kernel_node(const char* word);  // box, text, image, hit
bool kimb_kernel_flag(const char* word);  // fill, clip, scroll, disabled, selected, hidden, the alignments
bool kimb_state(const char* word);        // hover, pressed, focused, disabled, selected
// A declared type by name: number, color, string, size, any. False for one it does not know.
bool kimb_type_of(const char* name, KimbType* out);
const char* kimb_type_name(KimbType t);
// Whether `v` is a literal of the type, with no theme.
bool kimb_literal(KimbType t, const JsonValue& v);

// The value a property takes in a theme: a literal of its type as it is, else a token
// of the theme (the token's value, which must itself be a literal). Null, with why, when it
// is neither.
const JsonValue* kimb_resolve_value(const KimbScope& s, const char* theme, KimbType t, const JsonValue& v, const char** why);

// ---- the check ---------------------------------------------------------------------------

// Every mistake in the scope's first file, by the rules Krome loads it by: words that are
// neither the kernel's nor a definition, styles and tokens that are not there (a token
// must be in every theme the scope has, since a theme can be switched to), properties a
// node does not take, values of the wrong type, a definition used with parameters it
// does not take or children it has no slot for. True when there were none.
bool kimb_check(const KimbScope& s, KimErrors& errors);

} // namespace kv
