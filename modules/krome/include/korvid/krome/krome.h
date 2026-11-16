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
#include "korvid/core/name.h"
#include "korvid/kimb/kimb.h"
#include "korvid/krome/module.h"
#include "korvid/krome/tree.h"

namespace kv {

struct Engine;


// Out: a node's event fired (a press, or the script's emit). Name hashes; node is the id, 0
// when the node has none or the script emitted it.
// x and y: where the pointer was, logical pixels in the window, for a right-button `context`
// event (0 for the rest).
struct KromeEvent { u64 screen; u64 node; u64 event; f64 value; f32 x = 0.0f, y = 0.0f; };

// In: a model value from outside. kind: 0 a number, 1 a boolean (number 0 or 1), 2 a string.
constexpr u32 KROME_SET_TEXT = 56;
struct KromeSet { u64 screen; u64 key; f64 number; u32 kind; u32 pad; char text[KROME_SET_TEXT]; };

constexpr i32 KROME_DEFAULT_LAYER = 100;

// Opens the script at an asset path as a screen named by that path, read through the I/O
// queue and run when it lands. Opening a name already open replaces it.
Name krome_open(Engine& e, const char* script, i32 layer = KROME_DEFAULT_LAYER);
// The screen named `screen` in a .kimb at an asset path (docs/PRDs/Krome-Slice-2-PRD.md):
// the file and the preludes it uses read through the I/O queue, its `script` lines read and
// run, its tree built once all are in. Its name is "<file>#<screen>".
Name krome_open(Engine& e, const char* file, const char* screen, i32 layer = KROME_DEFAULT_LAYER);
// Bytes for an asset path that Krome takes instead of reading it: a .kimb, a prelude, a
// script. Copied. For tests, and for a game that compiles its own markup (the kim module).
void krome_provide(Engine& e, const char* path, const void* data, usize size);
// Every screen in this theme, from the next frame: its looks resolved again from the
// documents Krome keeps, nothing read again. Null: each screen's first theme.
void krome_theme(Engine& e, const char* theme);
// The theme a screen is in; null while it loads, or when its scope has none.
const char* krome_current_theme(Engine& e, Name screen);
// Nothing is loading, every font and image a screen asked for is in, and every screen has
// been laid out and painted since it last changed: what a picture of it should wait for.
bool krome_settled(Engine& e);
// The mistakes found loading and building screens, with their files and lines (the latest
// KV_KIMB_MAX_ERRORS; all are counted in KromeStats::script_errors and logged).
const KimErrors* krome_errors(Engine& e);
void krome_clear_errors(Engine& e);
// The same from a source in memory (copied), under a name of the caller's.
Name krome_open_source(Engine& e, const char* name, const char* source, i32 layer = KROME_DEFAULT_LAYER);
void krome_close(Engine& e, Name screen);
// Its script has run and its tree is built (false while it loads, and when it failed).
bool krome_ready(Engine& e, Name screen);

// Model values from C++: each an event, applied by Krome's pump before the tick's Sim.
void krome_set(Engine& e, Name screen, const char* key, f64 value);
void krome_set(Engine& e, Name screen, const char* key, const char* value);
void krome_set_bool(Engine& e, Name screen, const char* key, bool value);
// A list or a table from C++: a KBOR document (JsonBuilder and kbor_write make one), set
// as the Lua value it decodes to by the pump, in order with the others. An each over the
// key is built again.
void krome_set_doc(Engine& e, Name screen, const char* key, const void* kbor, usize size);

// Fires the node with this id as a press would: its Lua handler, then KromeEvent. False when
// there is no such screen or node, or it has no event. For tests, demos and the CLI.
bool krome_press(Engine& e, Name screen, const char* id);

// The screen's tree, for reading (layout, tests); null when it is not open.
const KromeTree* krome_tree(Engine& e, Name screen);
// A node by id; KROME_NONE when none.
u32 krome_find(const KromeTree& t, Name id);
// Where the node with this id is, in logical pixels, on the screen or the topmost of its
// overlays that has it. False when none does.
bool krome_find_rect(Engine& e, Name screen, const char* id, rect2* out);
// A model value as text, as {key} shows it; "" when unset. Valid until the next call.
const char* krome_model_text(Engine& e, Name screen, const char* key);

// The view's size when there is no window to ask (a test, a tool drawing off screen):
// physical pixels and the scale they are of logical ones. Once set, the window is not read.
void krome_viewport(Engine& e, f32 width, f32 height, f32 scale);

// What the view has done over the run: layouts, lists painted, lists handed to the renderer.
struct KromeStats { u64 layouts = 0, paints = 0, submits = 0, fired = 0, script_errors = 0, rebuilt_regions = 0, rebuilt_nodes = 0; };
KromeStats krome_stats(Engine& e);

} // namespace kv
