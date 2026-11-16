// Korvid svg: SVG files read into patherizer's shapes (docs/PRDs/Krome-Slice-5-PRD.md, "The SVG
// tool"). What icon sets and authoring tools write: shapes, paths, transforms, groups, `use`,
// clip paths, gradients, presentation attributes and `style="..."`. Anything outside that
// subset (text, filters, masks, patterns, markers, images, CSS stylesheets, animation) is
// refused by name, with its line, never dropped without a word; `lenient` makes the refusals
// warnings and leaves those parts out. goldberg runs it in process to cook `.svg` into
// `.kshape`; kvscript has it as `kv.svg`; a game may link it to read SVG itself.
//
//   PathShape shape;
//   shape.init(mem_general());
//   SvgMessages msgs;
//   if (!svg_to_shape(text, len, SvgOptions{}, shape, msgs)) { for each msgs.items[i]: print it }
#pragma once
#include "korvid/core/array.h"
#include "korvid/patherizer/patherizer.h"

namespace kv {

// The converter's version: a cook keys its steps by it, so a change here cooks every .svg again.
constexpr u32 KV_SVG_VERSION = 1;

struct SvgOptions {
    bool lenient = false; // refusals are warnings, and what they name is left out
};

struct SvgMessage {
    u32  line = 0;    // the source's line; 0 for the file as a whole
    bool error = true;
    char text[200] = {};
};

struct SvgMessages {
    Array<SvgMessage> items;
    u32 errors = 0;
    void free() { items.free(); }
    // "name:line: text" (or "name: text") for message i.
    void format_one(u32 i, const char* name, char* out, usize cap) const;
};

// The SVG text into `out` (cleared first). False when there was an error; `msgs` has every
// error and warning, each with its line.
bool svg_to_shape(const char* text, usize len, const SvgOptions& options, PathShape& out, SvgMessages& msgs);

} // namespace kv
