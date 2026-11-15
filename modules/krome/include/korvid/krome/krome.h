// Korvid krome: screens as Lua scripts, a model, and named events (docs/PRDs/Krome-Slice-1-PRD.md,
// "Lua, the model and events"). Krome holds one Lua state of its own; nothing outside it
// touches its tables. What crosses in or out is an engine event: KromeEvent out, KromeSet in.
//
//   Name form = krome_open(e, "kromedemo/form.lua");   // read and run when the I/O lands
//   krome_set(e, form, "player", "Rowan");              // arrives as a KromeSet; {player} in text follows
//   // a system:   for (const KromeEvent& ev : ctx.received<KromeEvent>()) if (ev.event == name_intern("save").hash) ...
//
// A screen script runs in an environment of its own (its globals over the shared ones) and
// returns its tree, or a screen table: { root = tree, model = { initial values }, on = {
// event = function(id, value) ... end } }. The tree is built with the helpers every script
// sees (row, col, stack, box, text, image, hit, spacer), in the shape .kimb will decode to:
//
//   return { model = { name = "?" }, on = { save = function(id) model.saved = true end },
//            root = col { pad = 12, gap = 8, text "Name: {name}",
//                         hit { id = "save", event = "save", text "Save" } } }
//
// Krome is cosmetic: none of its state is hashed, and its events are traced, not hashed.
#pragma once
#include "korvid/core/module.h"
#include "korvid/core/name.h"
#include "korvid/krome/tree.h"

namespace kv {

struct Engine;

KV_MODULE_DECLARE(krome_module);

// Out: a node's event fired (a press, or the script's emit). Name hashes; node is the id, 0
// when the node has none or the script emitted it.
struct KromeEvent { u64 screen; u64 node; u64 event; f64 value; };

// In: a model value from outside. kind: 0 a number, 1 a boolean (number 0 or 1), 2 a string.
constexpr u32 KROME_SET_TEXT = 56;
struct KromeSet { u64 screen; u64 key; f64 number; u32 kind; u32 pad; char text[KROME_SET_TEXT]; };

constexpr i32 KROME_DEFAULT_LAYER = 100;

// Opens the script at an asset path as a screen named by that path, read through the I/O
// queue and run when it lands. Opening a name already open replaces it.
Name krome_open(Engine& e, const char* script, i32 layer = KROME_DEFAULT_LAYER);
// The same from a source in memory (copied), under a name of the caller's.
Name krome_open_source(Engine& e, const char* name, const char* source, i32 layer = KROME_DEFAULT_LAYER);
void krome_close(Engine& e, Name screen);
// Its script has run and its tree is built (false while it loads, and when it failed).
bool krome_ready(Engine& e, Name screen);

// Model values from C++: each an event, applied by Krome's pump before the tick's Sim.
void krome_set(Engine& e, Name screen, const char* key, f64 value);
void krome_set(Engine& e, Name screen, const char* key, const char* value);
void krome_set_bool(Engine& e, Name screen, const char* key, bool value);

// Fires the node with this id as a press would: its Lua handler, then KromeEvent. False when
// there is no such screen or node, or it has no event. For tests, demos and the CLI.
bool krome_press(Engine& e, Name screen, const char* id);

// The screen's tree, for reading (layout, tests); null when it is not open.
const KromeTree* krome_tree(Engine& e, Name screen);
// A node by id; KROME_NONE when none.
u32 krome_find(const KromeTree& t, Name id);
// A model value as text, as {key} shows it; "" when unset. Valid until the next call.
const char* krome_model_text(Engine& e, Name screen, const char* key);

} // namespace kv
