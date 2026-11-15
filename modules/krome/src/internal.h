// Krome's insides: the module's state, a screen, and the Lua side's entry points.
#pragma once
#include "korvid/krome/krome.h"
#include "korvid/core/array.h"
#include "korvid/core/handle.h"
#include "korvid/render/ui.h"

struct lua_State;

namespace kv {

struct TickContext;

// A node whose shown text reads a model key.
struct KromeDep { u32 node; u64 key; };

// Where an overlay goes against its anchor.
enum class KromeSide : u8 { Below, Above, Right, Left, Pointer };

// A subtree opened above a screen (docs/PRDs/Krome-Slice-1-PRD.md, "Input and overlays").
struct KromeOverlay {
    u32       id = 0;
    KromeTree tree;
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
    int        env = -2, store = -2, on = -2; // registry refs (LUA_NOREF is -2)
    Handle     io;              // its script's read, while it loads
    char*      source = nullptr; // a source given in memory, until it runs
    usize      source_len = 0;
    bool       loading = false, ready = false, failed = false;
    Array<KromeOverlay*> overlays; // bottom first
    u32        next_overlay = 1;
    bool       repaint = true;      // its look changed (a hover, an overlay) since it was painted
};

// A font at one pixel size, as text at a size and a scale asks for it.
struct KromeFont { Name path; u32 px; Handle asset; bool ready; };
struct KromeImage { Name path; Handle asset; bool ready; };

struct KromeModule {
    Engine*             e = nullptr;
    Allocator*          general = nullptr;
    lua_State*          L = nullptr;
    Array<KromeScreen*> screens;
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

// ---- the Lua side (lua.cc) ----------------------------------------------------------------------

bool krome_lua_open(KromeModule& s);
void krome_lua_close(KromeModule& s);
// Runs a screen's script and builds its tree; false (logged) when it did not run.
bool krome_lua_run(KromeModule& s, KromeScreen& sc, const char* source, usize len, const char* chunk);
void krome_lua_release(KromeModule& s, KromeScreen& sc); // its refs
// A model value, from Lua or from outside: the store, then every text that reads it.
void krome_model_set_number(KromeModule& s, KromeScreen& sc, const char* key, f64 v, bool boolean);
void krome_model_set_string(KromeModule& s, KromeScreen& sc, const char* key, const char* v);
void krome_model_changed(KromeModule& s, KromeScreen& sc, u64 key);
const char* krome_model_value_text(KromeModule& s, KromeScreen& sc, const char* key, char* out, usize cap);
// A node's event: its handler, then KromeEvent. False when it has none.
bool krome_fire(KromeModule& s, KromeScreen& sc, KromeTree& tree, u32 node, f64 value);
void krome_emit(KromeModule& s, const KromeEvent& ev);
// An overlay from the node table at the top of the Lua stack (lua.cc), already added to sc.
bool krome_lua_build_overlay(KromeModule& s, KromeScreen& sc, KromeOverlay& o);

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
