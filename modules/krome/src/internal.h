// Krome's insides: the module's state, a screen, and the Lua side's entry points.
#pragma once
#include "korvid/krome/krome.h"
#include "korvid/core/arena.h"
#include "korvid/core/array.h"
#include "korvid/core/handle.h"
#include "korvid/kimb/kimb.h"
#include "korvid/typeset/typeset.h"
#include "korvid/platform_hid/input.h"
#include "korvid/render/ui.h"

struct lua_State;

namespace kv {

struct TickContext;

// A node that reads a model key: its shown text, its selected state (a binding), or one of
// its properties (`w=@key`: `prop` in look `state`, read from `path`).
struct KromeDep {
    enum Kind : u8 { Text, Binding, Prop, Behave }; // Behave: a behaviour's update, when its bound key changes
    u32  node;
    u64  key;
    Kind kind = Text;
    u8   state = 0;
    Name prop{}, path{};
};

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
    // A virtualized each (`height=24`): its elements [first, first + want) are built, those
    // before and after stood in for by spacers; len the list's length when it was built.
    f32               height = 0.0f;
    u32               first = 0, want = 0, len = 0;
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

// What the pointer is on: a screen's node in its tree (overlay 0) or in an overlay's.
struct KromeTarget {
    u32 screen = 0;   // the screen's id; 0 none
    u32 overlay = 0;  // the overlay's id; 0 the screen's own tree
    u32 node = KROME_NONE;
    bool operator==(const KromeTarget& o) const { return screen == o.screen && overlay == o.overlay && node == o.node; }
};

// A subtree opened above a screen (docs/PRDs/Krome-Slice-1-PRD.md, "Input and overlays").
struct KromeOverlay {
    u32       id = 0;
    KromeTree tree;
    Arena     arena;             // its tree as a document (from the Lua table), for rebuilding
    KimbFile  file;
    Name      anchor;            // the node it opens at, by id; null: the pointer or the window's center
    KromeSide side = KromeSide::Below;
    bool      modal = false;     // takes all input until closed; else it closes on a press outside
    bool      passive = false;   // a tooltip: the pointer goes through it, and nothing closes it but its owner
    color     dim{0, 0, 0, 0};   // drawn over the whole window under it
    vec2      at;                // the pointer when it opened, logical pixels
    bool      placed = false;    // laid out since it opened or the screen changed
    KromeTarget owner;           // the node whose behaviour opened it (w:open): its picks go there
    u32       anchor_node = KROME_NONE; // a node of the screen's tree it opens at, when it has no id
    bool      slot = false;      // built from a use's children (w:open_slot): closed when the screen is built again
};


// A definition's use with children whose body's root has a behaviour: those children, where
// they were written, for w:open_slot (a dropdown's options).
struct KromeSlot { u32 node; const KimbFile* file; const JsonValue* use; const KromeFrame* frame; };

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
    Array<KromeSlot> slots;
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
// An image node's asset: a texture, or a shape (a path to an .svg or a .kshape), which is
// rasterized at the size it is drawn (shapes.cc).
struct KromeImage { Name path; Handle asset; bool ready; bool shape = false; };

// The shapes' pages (shapes.cc): rasterized shapes packed as glyphs are, a texture each.
struct KromeShapePage {
    Handle texture;
    u8*    rgba = nullptr;     // w x h, sRGB, straight alpha
    u32    w = 0, h = 0;
    u32    x = 0, shelf_y = 0, shelf_h = 0;
    bool   own = false;        // one large shape's alone
    bool   dirty = false;
    u32    d0x = 0, d0y = 0, d1x = 0, d1y = 0;
    u64    used = 0;
};
struct KromeShapeEntry {
    Handle asset;
    u32    w = 0, h = 0;
    u32    colour = 0;         // currentColor's RGBA8, or all ones for a one-colour shape (drawn white)
    u32    page = 0, x = 0, y = 0;
    u64    used = 0;
};
// A large shape being rasterized on a task worker (shapes.cc): what it is, and the pixels it
// is drawn into until it lands in a page.
struct KromeShapeJob;

// The edit that has the keyboard (docs/PRDs/Krome-Slice-3-PRD.md, "Krome"): one across
// every screen. Its text is the buffer here while it has the focus, written back to the
// tree and the model as it changes.
struct KromeFocus {
    u32         screen = 0, overlay = 0, node = KROME_NONE; // node KROME_NONE: nothing has it (any focusable node, not only an edit)
    bool        keyboard = false; // it came by Tab: a node that is not an edit shows its focused look only then
    Name        id;              // the edit's id, to find it again when its tree is built again
    TypesetEdit buf;
    vec2        scroll{0, 0};    // physical pixels its text is moved by, to keep the caret in view
    f32         want_x = -1.0f;  // the column Up and Down keep to; < 0: the caret's
    f64         blink_from = 0.0;
    bool        blink_on = true; // as last painted
    bool        changed = false; // since the frame's change event
    // A press or a drag on it, resolved when its text is next laid out (paint).
    bool        pending = false, pending_extend = false;
    u32         pending_clicks = 0;
    vec2        pending_at{0, 0};
    u32         clicks = 0;
    f64         click_time = 0.0;
    vec2        click_at{0, 0};
    u32         word_begin = 0, word_end = 0; // what a double or triple press selected, for a drag from it
    char        composing[64] = {};
    u32         compose_caret = 0;
    // What its text was last laid out with, for the keys that need lines (Up, Down, Home).
    const FontAsset* font = nullptr;
    TypesetParams params;
    vec2        origin{0, 0};    // where the laid-out text's (0, 0) was drawn, physical pixels
    f32         view_h = 0.0f;   // its inside's height, for Page Up and Down
    rect2       told{0, 0, 0, 0}; // where the input method was last told the caret is
};

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
    Array<KromeShapePage>  shape_pages;
    Array<KromeShapeEntry> shape_entries;
    u64                 shape_clock = 0;
    u32                 shape_pages_made = 0, shapes_drawn = 0; // shapes_drawn: rasterizations, for the tests and the notes
    Array<KromeShapeJob*>  shape_jobs;       // large shapes on their way, landed by krome_shapes_land
    u32                 shape_jobs_run = 0, shapes_stood_in = 0; // jobs started; draws by another size or colour while one ran
    usize               shape_own_budget = 64u << 20; // the bytes the large shapes' own pages may keep
    u64                 shape_frame = 0;  // shape_clock when this frame began: a page used since is not freed
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
    // A scrollbar the pointer pressed: its box, which way, where on the thumb it was taken
    // (logical pixels from the thumb's start; < 0 a press on the track, which pages).
    struct Bar { KromeTarget box; bool horizontal = false; f32 grab = 0.0f; bool active = false; };
    Bar                 bar;
    // A tooltip: the node the pointer rests on that has one, since when, and the overlay
    // showing it (0 none yet). After a press it stays shut until the pointer leaves.
    KromeTarget         tip;
    f64                 tip_since = 0.0;
    u32                 tip_overlay = 0;
    bool                tip_shut = false;
    f64                 tip_delay = 0.6;  // seconds at rest before it shows (tests shorten it)
    KromeFocus          focus;
    // Behaviours: those registered from C++, the state each keeps for a node, and the Lua
    // side's (registry refs: name to its table of calls; a node's key to its state table).
    struct Behaviour { Name name; KromeBehaviourDesc desc; };
    Array<Behaviour>    behaviours;
    struct Blob { u32 screen, overlay, node; Name id, behave; void* data; u32 size; };
    Array<Blob>         blobs;
    int                 lua_behaviours = -2, lua_states = -2, lua_widget_meta = -2;
    Array<Name>         ran_scripts;  // preludes' scripts run, by asset path: once per state
    KromeTarget         hover_behave; // the node with a behaviour the pointer is over
    // Transitions (`transition=0.15`): for each node that has one, the colors it eases from
    // and to, and since when, on the Frame cycle's clock.
    struct Colors { color bg, bg2, border, shadow, text; f32 opacity; };
    struct Anim { u32 screen, overlay, node; Name id; Colors from, to; f64 start; f32 dur; };
    Array<Anim>         anims;
    f64                 clock = 0.0;      // the Frame cycle's dt summed
    bool                animating = false; // a transition was mid-way when last painted
    vec2                drag_at{-1.0f, -1.0f}; // where a behaviour's drag was last told
    bool                window_focused = true;
    u8                  cursor = 0;   // the shape last set (CursorShape + 1; 0 none yet)
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
// A tooltip's tree into an overlay: its text in std's `tooltip` style where the scope has
// one, else a plain dark box.
bool krome_tooltip_overlay(KromeModule& s, KromeScreen& sc, KromeOverlay& o, const char* text);
// An overlay's tree, from its document, in the screen's scope.
bool krome_build_overlay(KromeModule& s, KromeScreen& sc, KromeOverlay& o);
// A use's children into an overlay's tree, under a column box of `style`.
bool krome_build_slot(KromeModule& s, KromeScreen& sc, KromeOverlay& o, const KromeSlot& slot, const char* style);
const char* krome_theme_of(KromeModule& s, const KimbScope& scope);
// Each node that reads the model shows its text with the values filled in.
void krome_fill_node(KromeModule& s, KromeScreen& sc, KromeTree& tree, u32 node);
// A model key changed: the texts and bindings that read it follow, the regions that read
// it are built again.
void krome_rebuild_regions(KromeModule& s, KromeScreen& sc, u64 key);
// The virtualized eaches of a laid-out screen built again where what is in view has left
// what was built. True when one was: lay it out again.
bool krome_virtual_regions(KromeModule& s, KromeScreen& sc);

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
// A named event of a node's (an edit's change or blur, its submit): its handler, then KromeEvent.
void krome_fire_named(KromeModule& s, KromeScreen& sc, KromeTree& tree, u32 node, Name event, f64 value);
// A right-button press's event: the handler gets (id, x, y), KromeEvent the place.
void krome_fire_at(KromeModule& s, KromeScreen& sc, KromeTree& tree, u32 node, Name event, f32 x, f32 y);
// The model at a dotted path set to a string, and what reads it told.
void krome_model_write_string(KromeModule& s, KromeScreen& sc, const char* path, const char* value);

// ---- editing (edit.cc) ----------------------------------------------------------------------------

bool krome_is_focused(const KromeModule& s, u32 screen, u32 overlay, u32 node);
// An edit takes the focus: its text into the buffer, the caret at the end.
void krome_edit_take(KromeModule& s, KromeScreen& sc, u32 overlay, u32 node);
// A press on an edit: it takes the focus (if it had not), and the caret goes where the press
// was (a double press a word, a triple a line), once its text is laid out.
void krome_edit_press(KromeModule& s, KromeScreen& sc, u32 overlay, u32 node, f32 x, f32 y, bool shift);
void krome_edit_drag(KromeModule& s, f32 x, f32 y);
// The focus leaves what has it: an edit's text written back, its change and blur events
// fired, a behaviour's focus(false) called.
void krome_edit_blur(KromeModule& s);
// What the frame brought: keys, typed text, composition. True when it took the key.
bool krome_edit_key(KromeModule& s, Key key, bool shift, bool ctrl, bool alt, bool gui);
void krome_edit_typed(KromeModule& s, const char* utf8);
void krome_edit_compose(KromeModule& s, const char* text, u32 caret);
// The end of the frame's input: the change event, the blink's next frame.
void krome_edit_frame(KromeModule& s);
// A screen's tree was built again: the focused edit found again by its id, or blurred.
void krome_edit_rebuilt(KromeModule& s, KromeScreen& sc);
// The node the focus is on, in its tree, or null.
KromeTree* krome_focus_tree(KromeModule& s, KromeScreen** sc);
void krome_emit(KromeModule& s, const KromeEvent& ev);
// The model at a dotted path ("assets.3.name": a key, then an element, then a field), as
// `if` (truthy), `each` (a list's length) and text read it.
bool krome_model_truthy(KromeModule& s, KromeScreen& sc, const char* path);
u32  krome_model_length(KromeModule& s, KromeScreen& sc, const char* path);
const char* krome_model_path_text(KromeModule& s, KromeScreen& sc, const char* path, char* out, usize cap);
// The model's value at a path as a property takes it: a number, a boolean as 0 or 1, a
// string parsed as markup's are ("40%", "10..90", "#ff0000"), a list of up to four numbers.
// A string lands in `buf`. False when there is none (nil, a table that is not numbers).
bool krome_model_value(KromeModule& s, KromeScreen& sc, const char* path, KromeValue* out, char* buf, usize cap);
// A bound property set from the model as it is now (build.cc); the property's complaint, or null.
const char* krome_apply_bound(KromeModule& s, KromeScreen& sc, KromeTree& tree, const KromeDep& d);
// A string as markup's values are read: a number, a range, or the string itself.
void krome_string_value(const char* s, KromeValue* out);
// The model's text at a path into a tree node's text, whatever its length (an edit's).
void krome_model_into_text(KromeModule& s, KromeScreen& sc, const char* path, KromeTree& tree, u32 node);
// Whether a bound node is selected: its path truthy, or for `@key=value` the model's text
// there equal to the value.
bool krome_bound_selected(KromeModule& s, KromeScreen& sc, const KromeTree& tree, u32 node);
// A binding's press: the value at the path flipped, and what reads it told.
void krome_model_flip(KromeModule& s, KromeScreen& sc, const char* path);
// A KBOR document as a Lua value at a model key (krome_set_doc).
bool krome_model_set_doc(KromeModule& s, KromeScreen& sc, const char* key, const void* data, usize size);

// ---- behaviours (behave.cc) ------------------------------------------------------------------------

enum class KromeCall : u8 { Press, Drag, Release, Wheel, Key, Focus, Hover, Update, Pick };
struct KromeCallArgs {
    f32 x = 0.0f, y = 0.0f;       // physical pixels in the window (made the node's own logical ones)
    u32 button = 1;
    f32 dy = 0.0f;
    Key key = Key::Unknown;
    u32 mods = 0;
    bool on = false;
    const char* id = nullptr;   // a pick's: the node picked, and its value
    const char* text = nullptr;
};
// The node's behaviour called. False when it has none, or no such call; a key's or the
// wheel's, when it did not take it.
bool krome_behave(KromeModule& s, const KromeTarget& t, KromeCall call, const KromeCallArgs& a);
bool krome_behaviour_known(KromeModule& s, Name name);
// A node fired in an overlay a behaviour opened: its pick, with the node's id and value (its
// @key=value's value, else its id, else its text). True when the pick took it.
bool krome_pick(KromeModule& s, KromeScreen& sc, KromeTree& tree, u32 node);
// The descendant of a node named `part`, or KROME_NONE.
u32  krome_find_part(const KromeTree& t, u32 node, Name part);
// A part's property (or flag, given a boolean) set by a behaviour; repaint and layout follow.
void krome_set_part(KromeModule& s, const KromeTarget& t, const char* part, const char* prop, const KromeValue* v, i32 flag);
// The use's children (KromeSlot) opened as an overlay below the node, in a box of `style`
// (null: none). Its id, 0 when the node has none.
u32  krome_open_slot(KromeModule& s, const KromeTarget& t, const char* style);
// The overlays a node's behaviour opened, closed.
void krome_close_owned(KromeModule& s, const KromeTarget& t);
// The focus moved by `delta` through what is focusable in the top overlay the node opened.
void krome_focus_owned(KromeModule& s, const KromeTarget& t, i32 delta);
// The scripts of the preludes in a scope, each run once per state: true when all have run,
// false while one is still loading.
bool krome_prelude_scripts(KromeModule& s, const KimbScope& scope);
void krome_free_behaviours(KromeModule& s);
// The Lua side (lua.cc): a prelude's script run, its behaviours into the registry; the call.
bool krome_lua_prelude(KromeModule& s, const char* source, usize len, const char* chunk);
bool krome_lua_has_behaviour(KromeModule& s, Name name);
bool krome_lua_behave(KromeModule& s, KromeScreen& sc, KromeTree& tree, u32 overlay, u32 node, KromeCall call, f32 x, f32 y, const KromeCallArgs& a, bool* taken);
// A key as a behaviour sees it: "left", "enter", "a".
const char* krome_key_name(Key k, char* out, usize cap);
// The model at a path as a number; a Lua value written there (the top of the stack, popped).
bool krome_model_number(KromeModule& s, KromeScreen& sc, const char* path, f64* out);
void krome_model_write_number(KromeModule& s, KromeScreen& sc, const char* path, f64 v);
void krome_model_write_value(KromeModule& s, KromeScreen& sc, const char* path, int idx); // the Lua value at idx

// ---- keyboard focus (focus.cc) ----------------------------------------------------------------------

// Whether Tab and a press give the node the focus: an edit, a hit, anything with an event, a
// binding or a behaviour, or `focusable`; not `nofocus`, nor what is disabled or hidden.
bool krome_focusable(const KromeTree& t, u32 node);
// The focus to the node (an edit's through krome_edit_take); `keyboard` when Tab brought it.
void krome_focus_take(KromeModule& s, KromeScreen& sc, u32 overlay, u32 node, bool keyboard);
// Tab (back with Shift): the next focusable node of the focus's screen, or of the top one; a
// modal's only while one is open. False when an edit keeps Tab (`tabs`).
bool krome_focus_tab(KromeModule& s, bool back);
// A key to what has the focus when it is not an edit: its behaviour, else Space and Enter
// fire it; Page Up and Down, Home and End move the scroll box it is in. True when taken.
bool krome_focus_key(KromeModule& s, Key key, u32 mods);
// A shortcut (`key="mod+s"`) of the top screen's: its node fired. True when one matched.
bool krome_shortcut(KromeModule& s, Key key, u32 mods);
// The tree the focused node is in, whatever it is; null when nothing has the focus.
KromeTree* krome_focused_tree(KromeModule& s, KromeScreen** sc);

// ---- the view (view.cc) --------------------------------------------------------------------------

KromeOverlay* krome_overlay(KromeScreen& sc, u32 id);
// The overlay whose tree this is, or null (the screen's own).
KromeOverlay* krome_overlay_of(KromeScreen& sc, const KromeTree& t);
// The transitions of a screen's tree (overlay 0) or an overlay's dropped: built again, or gone.
void krome_drop_anims(KromeModule& s, u32 screen, u32 overlay);
// The screen on top (a higher layer, then the later opened); null when none is ready.
KromeScreen* krome_top_screen(KromeModule& s);
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
// The wheel at the pointer: the nearest scroll box under it that goes that way (dy on a
// box that scrolls only across moves it across).
void krome_wheel(KromeModule& s, f32 dx, f32 dy);
// A right-button press at (x, y), physical pixels: light overlays it is outside close, and
// the nearest node under it with a `context` event fires it.
void krome_context_press(KromeModule& s, f32 x, f32 y);
void krome_free_view(KromeModule& s);

// ---- shapes (shapes.cc) ----
struct ShapeAsset;
struct UiBox;
// The box made to draw a shape at w x h pixels (the page, the uv): rasterized now if it is not
// in a page yet. `current` is currentColor for a shape with colours of its own; a one-colour
// shape is drawn white and the caller tints the box. False when it cannot.
bool krome_shape_box(KromeModule& s, Renderer* r, const ShapeAsset& shape, Handle asset, u32 w, u32 h, color current, UiBox& box);
// A shape of more than 256 x 256 pixels goes to a task worker of the platform's queue instead,
// and the box is false (or another size or colour of the same shape, stretched, while it runs).
// Once a frame, before the paint: the jobs that have finished, into their pages; true when one
// landed.
bool krome_shapes_land(KromeModule& s, Renderer* r);
void krome_shapes_flush(KromeModule& s, Renderer* r); // the pages' changed rectangles, to the renderer
void krome_shapes_free(KromeModule& s, Renderer* r);

} // namespace kv
