// Korvid save: game state as named fields. Any plain struct with a field table (every
// component, every event) is written as an object of its fields by name, and read back
// the same way: a field the file lacks keeps the value it had, a key the struct lacks is
// ignored, so a save outlives fields being added and removed. A kind is written slot by
// slot, generations and the free list included, and restored exactly: the handles that
// pointed into it before the save point to the same entities after the load, and the
// next spawn hands out the handle it would have.
//
// Handles are written by the kind's name, never its id (an id is only the order kinds
// registered in), and then by the handle's own name when its issuer gives it one
// ("render.material@example_arcanum/hero.png:alpha", "level.root@level.root"), or by slot
// and generation ("game.npc#3:1"). A named handle is found by name on load, so it means
// the same thing in a run that made things in another order; one by slot is remade with
// today's kind id, which is right for kinds the save restores. For anything else the game
// passes a SaveRemap and answers itself.
//
//   SaveWriter w; w.init(mem_general());
//   w.begin_object();
//   w.key("level"); w.string_value("village");
//   w.key("kinds"); save_kinds(w, e, kinds);       // {"game.player": {...}, "game.npc": {...}}
//   w.end_object();
//   ...
//   JsonDoc doc; save_parse(SaveFormat::Json, bytes, size, mem_scratch(), doc);
//   load_kinds(doc, *json_get(doc, doc.root(), "kinds"), e, remap);
#pragma once
#include "korvid/core/kind.h"
#include "korvid/save/writer.h"

namespace kv {

struct Engine;

// A handle as the file had it: by name ("kind@name") or by slot ("kind#index:gen").
struct SavedHandle {
    const char* kind;  // the kind's name
    const char* name;  // the handle's name, or null for one saved by slot
    u32         index;
    u32         gen;
};
constexpr u32 KV_SAVE_HANDLE_MAX = 256; // bytes of a handle's text, NUL included

// Called for every non-null handle read back. `remade` is the default answer: today's kind
// of that name with the saved index and generation, or null when no kind has the name.
struct SaveRemap {
    Handle (*fn)(const SavedHandle& saved, Handle remade, void* user) = nullptr;
    void*  user = nullptr;
};

// One handle: null, or "kind#index:gen".
void   save_handle(SaveWriter& w, Handle h);
Handle load_handle(const JsonValue* v, const SaveRemap& remap = SaveRemap{});
// "kind@name" or "kind#index:gen" into its parts; the kind is copied into `kind` (cap
// bytes), `name` points into `text`. False when the text is not one.
bool   parse_handle(const char* text, SavedHandle* out, char* kind, u32 cap);

// A row as an object of its fields: a scalar as itself, a vector as an array of lanes, a
// Char field as a string, a handle as above.
void save_fields(SaveWriter& w, Span<const FieldDesc> fields, const void* row);
// The fields `obj` names, into `row`; the rest keep their values. A value of the wrong
// shape is skipped with a warning. Returns the fields read.
u32  load_fields(const JsonDoc& doc, const JsonValue& obj, Span<const FieldDesc> fields, void* row, const SaveRemap& remap = SaveRemap{});

inline void save_component(SaveWriter& w, const ComponentDesc& c, const void* row) { save_fields(w, c.fields, row); }
inline u32  load_component(const JsonDoc& doc, const JsonValue& obj, const ComponentDesc& c, void* row, const SaveRemap& remap = SaveRemap{}) {
    return load_fields(doc, obj, c.fields, row, remap);
}

// A whole kind:
//   {"free": [[slot, gen], ...],                    the dead slots, in reuse order
//    "rows": [{"slot": 0, "gen": 1, "Transform2": {...}, ...}, ...]}
// Every component is written, cosmetic ones too: a kind is saved because it is wanted back.
// A named entity's row has "name".
void save_kind(SaveWriter& w, const Table& t);
// Replaces the table's contents with the saved ones. Components the file lacks are zero;
// previous-tick copies equal the loaded rows (a loaded entity has no history, as a spawned
// one has none). With an engine, the entities get their names back (outside the schedule
// or from a main-thread unit, as engine_entity_name_set). False, with the table left
// empty, if the slots do not fit or disagree.
bool load_kind(const JsonDoc& doc, const JsonValue& v, Table& t, const SaveRemap& remap = SaveRemap{}, Engine* e = nullptr);

// Several kinds as one object keyed by the kinds' full names. Names that are not kinds
// are skipped with a warning.
void save_kinds(SaveWriter& w, Engine& e, Span<const char* const> kinds);
// Every kind `obj` names, each with load_kind and its names: first every kind's slots and
// names, then every kind's rows, so a handle saved by name into one kind finds an entity of
// another restored by the same load. A name with no table is skipped with a warning. Returns the kinds loaded; false-y (0) is also what an empty object gives.
u32  load_kinds(const JsonDoc& doc, const JsonValue& obj, Engine& e, const SaveRemap& remap = SaveRemap{});

} // namespace kv
