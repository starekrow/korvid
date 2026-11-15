// Krome's insides: the module's state, a screen, and the Lua side's entry points.
#pragma once
#include "korvid/krome/krome.h"
#include "korvid/core/arena.h"
#include "korvid/core/array.h"
#include "korvid/core/handle.h"
#include "korvid/kimb/kimb.h"
#include "korvid/render/ui.h"

struct lua_State;

namespace kv {

struct TickContext;

// A node that reads a model key: its shown text, or its selected state (a binding).
struct KromeDep { u32 node; u64 key; bool binding = false; };

// Where a definition's body or an each's block is being built: what its texts and
// paths see (docs/PRDs/Krome-Slice-2-PRD.md, "Definitions", "Control flow and the model").
// Kept for as long as the tree, so a region can be built again in the frame it was in.
struct KromeFrame {
    const KromeFrame* outer = nullptr;  // the frame the use (or the each) was written in
    // A definition's expansion.
    const KimbFile*  use_file = nullptr;
    const JsonValue* use = nullptr;     // its children are what the body's slot holds
    const KimbFile*  def_file = nullptr;
    const char*      names[16] = {};
    const char*      values[16] = {};
    u32              params = 0;
    // An each's element: `var` is the element at `path` in the model ("assets.3").
    const char*      var = nullptr;
    const char*      path = nullptr;
};

// An each or an if: the nodes after its anchor (a hidden node where it was written) are
// what it built, and are built again when the model key it reads changes.
struct KromeRegion {
    u64               key;     // the model key its path starts with
    u32               anchor;
    u32               count;   // top-level nodes after the anchor that are its own
    const KimbFile*   file;
    const JsonValue*  src;     // the each or the if
    const KromeFrame* frame;
    bool              alive;
    const KimbScope*  scope = nullptr; // a kim() piece's own, or the screen's
};

// A file Krome reads: a .kimb (a screen's, or a prelude a `use` names) or a script a
// `script` line names. Read once through the I/O queue, or taken from what krome_provide
// gave, and kept: a theme switch rebuilds from the documents, never from the disk.
struct KromeDoc {
    Name     path;            // the asset path read ("kromedemo/form.kimb", "kromedemo/form.lua")
    bool     script = false;  // a Lua source, not a .kimb
    Handle   io;
    bool     loading = true, ready = false, failed = false;
    Arena    arena;           // the document's nodes and strings
    KimbFile file;
    char*    source = nullptr; // a script's text
    usize    source_len = 0;
};

// What a screen is waiting for before it can be built.
enum class KromeStage : u8 { Read, Scripts, Uses, Built, Failed };

// Where an overlay goes against its anchor.
enum class KromeSide : u8 { Below, Above, Right, Left, Pointer };

// A subtree opened above a screen (docs/PRDs/Krome-Slice-1-PRD.md, "Input and overlays").
struct KromeOverlay {
    u32       id = 0;
    KromeTree tree;
    Arena     arena;             // its tree as a document (from the Lua table), for rebuilding
    KimbFile  file;
    Name      anchor;            // the node it opens at, by id; null: the pointer or the window's center
    KromeSide side = KromeSide::Below;
    bool      modal = false;     // takes all input until closed; else it closes on a press outside
    color     dim{0, 0, 0, 0};   // drawn over the whole window under it
    vec2      at;                // the pointer when it opened, logical pixels
    bool      placed = false;    // laid out since it opened or the screen changed
};

// What the pointer is on: a screen's node in its tree (overlay 0) or in an overlay's.
struct KromeTarget {
    u32 screen = 0;   // the screen's id; 0 none
    u32 overlay = 0;  // the overlay's id; 0 the screen's own tree
    u32 node = KROME_NONE;
    bool operator==(const KromeTarget& o) const { return screen == o.screen && overlay == o.overlay && node == o.node; }
};

struct KromeScreen {
    u32        id = 0;          // what Lua's closures name it by
    Name       name;
    i32        layer = KROME_DEFAULT_LAYER;
    KromeTree  tree;
    Array<KromeDep> deps;
    // Where its tree comes from: a screen of a .kimb (doc, screen), or a script's root
    // table made a document of its own (lua_file, in arena).
    KromeStage stage = KromeStage::Read;
    KromeDoc*  doc = nullptr;
    Name       screen;          // the screen in doc
    Name       script;          // a Lua screen's script, by asset path; null for one given as source
    Arena      arena;
    KimbFile   lua_file;
    bool       has_lua_file = false;
    Array<Name> uses;           // a Lua screen's `uses`
    KimbScope  scope;           // the file, then its uses: built once they are all in
    Arena      frames;          // the KromeFrames its tree was built in, until it is built again
    Array<KromeRegion> regions;
    u32        dead = 0;        // nodes rebuilt away since the tree was last built whole
    bool       waiting = false; // a kim() piece's file has not landed: built again when it does
    int        env = -2, store = -2, on = -2; // registry refs (LUA_NOREF is -2)
    Handle     io;              // its script's read, while it loads
    char*      source = nullptr; // a source given in memory, until it runs
    usize      source_len = 0;
    bool       loading = false, ready = false, failed = false;
    Array<KromeOverlay*> overlays; // bottom first
    u32        next_overlay = 1;
    bool       repaint = true;      // its look changed (a hover, an overlay) since it was painted
    bool       rebuild = false;     // its tree is to be built again from its document (a theme switch)
};

// A font at one pixel size, as text at a size and a scale asks for it.
struct KromeFont { Name path; u32 px; Handle asset; bool ready; };
struct KromeImage { Name path; Handle asset; bool ready; };

struct KromeModule {
    Engine*             e = nullptr;
    Allocator*          general = nullptr;
    lua_State*          L = nullptr;
    Array<KromeScreen*> screens;
    Array<KromeDoc*>    docs;
    // Files given from memory (krome_provide), by asset path: taken in place of a read.
    struct Provided { Name path; char* data; usize size; };
    Array<Provided>     provided;
    Name                theme;    // the theme every screen is in; null: each scope's first
    KimErrors           errors;   // the latest mistakes found loading and building (krome_errors)
    // Lists and tables from C++ (krome_set_doc), applied by the pump in order.
    struct SetDoc { Name screen; Name key; u8* data; usize size; };
    Array<SetDoc>       set_docs;
    u64                 rebuilt_regions = 0, rebuilt_nodes = 0;
    u32                 next_id = 1;
    TickContext*        ctx = nullptr; // the running system's, for emits; null outside one
    u64                 fired = 0, script_errors = 0;
    char                text[512] = {}; // krome_model_text's answer

    // The view: the window in physical pixels and its scale, or what a test or a tool with
    // no window said (krome_viewport).
    f32                 win_w = 0.0f, win_h = 0.0f, scale = 1.0f;
    bool                viewport_fixed = false;
    Array<KromeFont>    fonts;
    Array<KromeImage>   images;
    // The pointer: where, what is under it, what took the press, what the buttons were.
    f32                 px = -1.0f, py = -1.0f;
    KromeTarget         under;    // the deepest node under the pointer
    KromeTarget         captured; // the node a press went down on, until the release
    bool                down = false, escape = false;
    // The lists handed to the renderer, by layer: so a layer whose screens are gone is cleared.
    Array<i32>          layers;
    UiList              list;
    u64                 paints = 0, layouts = 0, submits = 0;
    u32                 input_id = 0; // input.state's resource id in this engine
    bool                warned_no_ui = false;
};

KromeModule*  krome_state(Engine& e);
KromeScreen* krome_screen(KromeModule& s, Name name);
KromeScreen* krome_screen_by_id(KromeModule& s, u32 id);

// A mistake with its place: logged as file:line: text (and the use, through a definition),
// kept in s.errors, counted in script_errors.
void krome_error(KromeModule& s, const char* file, u32 line, const char* used_file, u32 used_line, const char* fmt, ...) KV_PRINTF(6, 7);

// ---- documents (doc.cc) --------------------------------------------------------------------------

// The document at an asset path, asked for if it is not yet (and read in the pump).
KromeDoc* krome_doc(KromeModule& s, Name path, bool script);
KromeDoc* krome_doc_loaded(KromeModule& s, Name path);
// Reads and reaps what the queue finished for documents; requests what is wanted.
void krome_pump_docs(KromeModule& s);
void krome_free_docs(KromeModule& s);
// A use's file: "krome/std" is "krome/std.kimb".
Name krome_use_path(const char* use);
// Whether every file the scope needs has landed: true when built (into sc.scope), false
// while some are still loading; *failed when one never will.
bool krome_scope_ready(KromeModule& s, KromeScreen& sc, bool* failed);
// The same for any file: true with `scope` filled when every use has landed.
bool krome_file_scope(KromeModule& s, const KimbFile& f, KimbScope& scope, bool* failed);

// ---- building (build.cc) -------------------------------------------------------------------------

// The screen's tree, from its document, in its scope and the module's theme. Overlays too.
// What was scrolled stays scrolled, by id.
bool krome_build_screen(KromeModule& s, KromeScreen& sc);
// An overlay's tree, from its document, in the screen's scope.
bool krome_build_overlay(KromeModule& s, KromeScreen& sc, KromeOverlay& o);
const char* krome_theme_of(KromeModule& s, const KimbScope& scope);
// Each node that reads the model shows its text with the values filled in.
void krome_fill_node(KromeModule& s, KromeScreen& sc, KromeTree& tree, u32 node);
// A model key changed: the texts and bindings that read it follow, the regions that read
// it are built again.
void krome_rebuild_regions(KromeModule& s, KromeScreen& sc, u64 key);

// ---- the Lua side (lua.cc) ----------------------------------------------------------------------

bool krome_lua_open(KromeModule& s);
void krome_lua_close(KromeModule& s);
// Runs a screen's script: its model, its handlers, its uses, and its root made a document
// (sc.lua_file). False (logged) when it did not run.
bool krome_lua_run(KromeModule& s, KromeScreen& sc, const char* source, usize len, const char* chunk);
// A .kimb screen's scripts, in order, in one environment: their models merged, their
// handlers merged (one event handled twice is an error). False when one failed; what the
// others gave stands.
bool krome_lua_scripts(KromeModule& s, KromeScreen& sc, KromeDoc* const* scripts, u32 count);
void krome_lua_release(KromeModule& s, KromeScreen& sc); // its refs
// A model value, from Lua or from outside: the store, then every text that reads it.
void krome_model_set_number(KromeModule& s, KromeScreen& sc, const char* key, f64 v, bool boolean);
void krome_model_set_string(KromeModule& s, KromeScreen& sc, const char* key, const char* v);
void krome_model_changed(KromeModule& s, KromeScreen& sc, u64 key);
const char* krome_model_value_text(KromeModule& s, KromeScreen& sc, const char* key, char* out, usize cap);
// A node's event: its handler, then KromeEvent. False when it has none.
bool krome_fire(KromeModule& s, KromeScreen& sc, KromeTree& tree, u32 node, f64 value);
void krome_emit(KromeModule& s, const KromeEvent& ev);
// The model at a dotted path ("assets.3.name": a key, then an element, then a field), as
// `if` (truthy), `each` (a list's length) and text read it.
bool krome_model_truthy(KromeModule& s, KromeScreen& sc, const char* path);
u32  krome_model_length(KromeModule& s, KromeScreen& sc, const char* path);
const char* krome_model_path_text(KromeModule& s, KromeScreen& sc, const char* path, char* out, usize cap);
// A binding's press: the value at the path flipped, and what reads it told.
void krome_model_flip(KromeModule& s, KromeScreen& sc, const char* path);
// A KBOR document as a Lua value at a model key (krome_set_doc).
bool krome_model_set_doc(KromeModule& s, KromeScreen& sc, const char* key, const void* data, usize size);

// ---- the view (view.cc) --------------------------------------------------------------------------

KromeOverlay* krome_overlay(KromeScreen& sc, u32 id);
u32  krome_open_overlay(KromeModule& s, KromeScreen& sc); // a new, empty overlay on top; its id
void krome_close_overlay(KromeModule& s, KromeScreen& sc, u32 id); // 0: the top one
void krome_free_overlays(KromeModule& s, KromeScreen& sc);
KromeTree* krome_target_tree(KromeModule& s, const KromeTarget& t, KromeScreen** screen);
// The window's size and scale, from the window or what was fixed.
void krome_update_viewport(KromeModule& s);
// Lays out what changed and paints what must be; hands each changed layer's list over.
void krome_view_frame(KromeModule& s);
// The screen's list: its tree, then its overlays. For tests too.
void krome_paint_screen(KromeModule& s, KromeScreen& sc, UiList& list);
// The pointer and the keys as one sample sees them: hover, press, release, wheel, Escape.
void krome_pointer(KromeModule& s, f32 x, f32 y, bool down, f32 wheel, bool escape);
void krome_free_view(KromeModule& s);

} // namespace kv
