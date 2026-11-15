// Korvid ui: menus, HUDs, dialogue and screens, as a tree the module owns.
//
// A screen is a tree of named nodes, built from a document (.ui.json) or in code, laid
// out in UI units against a design height and drawn as cosmetic screen-space sprites
// (the kind "ui.sprite"): output for the renderer and the CLI, never input. Nodes:
//
//   box     a plain container: children placed by anchor, pivot and offset
//   panel   a box with a frame behind it: a 9-slice from the style, or a flat color
//   image   a rectangle of a texture, or a frame of a strip of them
//   label   rich text: wrap at the node's width, color, wave, shake, pauses, model
//           values, and a reveal (a typewriter) that a confirm completes
//   stack   children one after another in a column or a row, with a gap
//   list    a column of items with a focus: move, confirm and cancel from the action
//           map, a cursor, sounds, and UiEvents by the item's name
//   scroll  one child taller than itself, clipped to its rect, scrolled by input or code
//   field   a line of typed text with a caret: the topmost screen's first visible field
//           takes the keyboard's text (not the actions, so typing a letter moves nothing),
//           Backspace erases, Enter or a typed newline submits (UiEvent submit), Escape
//           cancels; "changed" as it is edited
//   meter   a value out of a maximum: a row of icons (pips) or a bar
//
// Layout. Each node has an anchor and a pivot (the nine points of a rect), an offset, and
// a size per axis: fixed (units), fit (its content) or fill (its parent's inside). Padding
// is inside, a stack's gap between children. The screen is the window in UI units: its
// height divided by the document's design height, rounded down to a whole number when the
// document says pixel (pixel art stays crisp), continuous otherwise.
//
// Rich text, in a label's text:
//   {*c=gold} ... {/c}   a color from the style, or #rrggbb / #rrggbbaa
//   {*wave} ... {/wave}  each letter bobs       {*shake} ... {/shake}  each letter jitters
//   {*pause=0.4}         the reveal waits       {key}  a model value (ui_model_set_*)
//   A tag starts with *, a closing with /; anything else in braces is a model value, as in
//   Krome's texts.
//   {{                  a brace                  \n      a new line
//
// Input and events. The topmost open screen that takes input reads the action map (the
// document's "actions" names move, confirm and cancel; those are the defaults). A list
// moves its focus with repeat; confirm on an item emits UiEvent{screen, item, select}
// (or the item's own event name), cancel emits cancel. A label still revealing takes the
// confirm and completes. With no list, confirm and cancel are emitted as themselves, so
// a dialogue pages on confirm. Events come from "ui.update" (PreSim, every tick), so a
// handler runs before that tick's Sim.
//
// Call the functions here from the main thread: handlers, main-thread systems, startup.
#pragma once
#include "korvid/core/handle.h"
#include "korvid/core/name.h"
#include "korvid/math/color.h"
#include "korvid/math/rect2.h"

namespace kv {

struct Engine;

constexpr u32 KV_UI_NONE = 0xFFFFFFFFu;

enum class UiType : u8 { Box, Panel, Image, Label, Stack, List, Scroll, Field, Meter, Count };
enum class UiSize : u8 { Fixed, Fit, Fill };
enum class UiDir : u8 { Column, Row };
enum class UiAlign : u8 { Start, Center, End };
// The nine points of a rect, as ScreenAnchor names them.
enum class UiPoint : u8 { Center, TopLeft, Top, TopRight, Left, Right, BottomLeft, Bottom, BottomRight };

const char* ui_type_name(UiType t);

// A node as code builds it; the document's JSON says the same things by the same names.
struct UiNodeDesc {
    UiType      type     = UiType::Box;
    const char* name     = "";            // unique among its siblings; paths are names joined by '/'
    UiPoint     anchor   = UiPoint::Center;
    UiPoint     pivot    = UiPoint::Center;
    vec2        offset;                   // units, +x right, +y down
    UiSize      w_mode   = UiSize::Fit, h_mode = UiSize::Fit;
    vec2        size;                     // for Fixed
    f32         padding[4] = {};          // left, top, right, bottom
    f32         gap      = 0.0f;          // stack, list, meter pips
    UiDir       dir      = UiDir::Column; // stack
    UiAlign     align    = UiAlign::Start;// stack and list: across; label: the text's lines
    bool        visible  = true;
    // label, list item
    const char* text     = "";
    const char* color    = "";            // a style color, or #rrggbb[aa]; "" is the style's "text"
    f32         font_scale = 1.0f;        // the style's font at this many times its size (whole numbers stay crisp)
    bool        wrap     = false;         // at the node's width
    f32         reveal   = 0.0f;          // characters per second; 0 shows the text at once
    const char* shadow   = "";            // a color for a drop shadow one unit down-right, "" for the style's (if any)
    // panel, image, meter
    const char* frame    = "";            // a style frame (panel), or "" for none
    const char* fill     = "";            // a panel's flat color, a bar meter's fill color
    const char* image    = "";            // a style image (image, pips)
    u32         frame_index = 0;          // an image strip's frame
    // meter: a number, or "=key" for a model value
    const char* value    = "";
    const char* max      = "";
    bool        bar      = false;         // a bar, not pips
    // list item, button, field
    const char* event    = "";            // what selecting it emits; "" is "select"
    u32         max_length = 24;          // field
    bool        input    = false;         // scroll: moved by the move action
};

// Activity, from ui.update. `node` and `event` are Name hashes (name_str prints them):
// select (or an item's event), cancel, confirm, revealed, changed, submit.
struct UiEvent { Handle screen; u64 node; u64 event; i32 value; u32 pad; };

// ---- documents --------------------------------------------------------------------------
// A .ui.json: a style and named screens. An asset (ui.doc) in the asset module's scopes,
// with its textures, sounds and fonts held by it. Ready when they are.
//
//   { "design": [480, 270], "pixel": true,
//     "actions": { "move": "move", "confirm": "confirm", "cancel": "cancel" },
//     "style": { "font": "mygame/ui/font.ttf", "size": 8, "line": 10, "space": 3,
//                "colors": { "text": "#ffffff", "gold": "#ffc840", "focus": "#ffc840" },
//                "shadow": "#000000a0", "shadow_offset": 1,
//                "frames": { "panel": { "texture": "mygame/ui/panel.png", "insets": [4, 4, 4, 4] } },
//                "images": { "cursor": { "texture": "mygame/ui/arrow.png", "rect": [0, 0, 13, 13] },
//                            "heart":  { "texture": "mygame/ui/heart.png", "rect": [0, 0, 16, 16], "frames": 5 } },
//                "sounds": { "move": "...", "select": "...", "cancel": "...", "tick": "..." } },
//     "screens": { "pause": { "type": "panel", "frame": "panel", "children": [ ... ] } } }
Handle ui_load(Engine& e, const char* path, Handle scope);
bool   ui_ready(Engine& e, Handle doc);
// Reads every loaded document again and rebuilds the screens open on them, focus kept by
// name. The CLI's `ui reload`.
u32    ui_reload(Engine& e);

// ---- screens -----------------------------------------------------------------------------
// How a screen comes and goes: at once, fading, or sliding in from an edge.
enum class UiFx : u8 { Cut, Fade, Slide, Iris };
struct UiMotion {
    UiFx    fx      = UiFx::Cut;
    f32     seconds = 0.25f;
    UiPoint from    = UiPoint::Bottom; // Slide: the edge it comes from (and goes back to)
};

struct UiOpen {
    i32      layer = 100;  // the sprite layer it draws in; its children above it in tree order
    bool     input = true; // takes the action map while it is the topmost that does
    bool     hold  = false;// holds the sim (HoldUi) while open
    UiMotion enter;        // how it arrives
};
// Opens a document's screen. The handle (ui.screen) is good at once; a document still
// loading opens the screen when it is Ready. Null for a screen the document lacks.
Handle ui_open(Engine& e, Handle doc, const char* screen, const UiOpen& open = UiOpen{});
// A screen built in code, styled by a document: ui_add its nodes, root first (parent
// KV_UI_NONE). Returns the node's index.
Handle ui_open_empty(Engine& e, Handle doc, const char* name, const UiOpen& open = UiOpen{});
u32    ui_add(Engine& e, Handle screen, u32 parent, const UiNodeDesc& desc);
bool   ui_close(Engine& e, Handle screen);
// Closes after a motion out (fade, slide); input leaves it at once.
bool   ui_close_with(Engine& e, Handle screen, const UiMotion& leave);

// ---- covers: switching between modes ----------------------------------------------------
// A cover closes over the whole window (a fade to a color, or an iris shrinking onto a
// point), says UiCovered when nothing of what was there shows, waits while the game holds
// it (ui_cover_wait), and opens again on whatever is there now, saying UiUncovered. The
// sim is held while it is up unless it says otherwise. This is where a game switches from
// the world to a minigame, from the title to the village, to a game-over screen: open and
// close screens, move the camera, load, in the UiCovered handler.
struct UiCover {
    UiFx  fx       = UiFx::Fade; // Fade or Iris
    f32   seconds  = 0.4f;       // each way
    color tint     = color{0, 0, 0, 1};
    vec2  focus{0.5f, 0.5f};     // Iris: where it closes, a fraction of the window from the top-left
    bool  hold     = true;
    u64   user     = 0;          // handed back in the events
};
struct UiCovered   { u32 cover; u32 pad; u64 user; };
struct UiUncovered { u32 cover; u32 pad; u64 user; };
u32  ui_cover(Engine& e, const UiCover& cover);
// While waiting, a cover stays closed after UiCovered (a level loading behind it).
void ui_cover_wait(Engine& e, u32 cover, bool wait);
bool ui_covering(Engine& e);

// A field's text, as typed so far ("" for none).
const char* ui_field_text(Engine& e, Handle screen, const char* path);
bool   ui_is_open(Engine& e, Handle screen);
// The topmost open screen that takes input, or null.
Handle ui_top(Engine& e);

// ---- nodes ---------------------------------------------------------------------------------
// By path from the screen's root: "box/items/save" ("" is the root). KV_UI_NONE for none.
u32  ui_find(Engine& e, Handle screen, const char* path);
bool ui_set_text(Engine& e, Handle screen, const char* path, const char* text);
bool ui_set_visible(Engine& e, Handle screen, const char* path, bool visible);
bool ui_set_offset(Engine& e, Handle screen, const char* path, vec2 offset);
bool ui_set_color(Engine& e, Handle screen, const char* path, const char* color);
// An image node's picture: a style image by name, or a texture path and a pixel rect.
bool ui_set_image(Engine& e, Handle screen, const char* path, const char* style_image, u32 frame = 0);
bool ui_set_texture(Engine& e, Handle screen, const char* path, const char* texture, rect2 px);
bool ui_set_value(Engine& e, Handle screen, const char* path, f32 value, f32 max);
// A list's focus, by its item's name.
bool ui_focus(Engine& e, Handle screen, const char* list, const char* item);
// A list's focused item's name, or null.
const char* ui_focused(Engine& e, Handle screen, const char* list);
bool ui_scroll(Engine& e, Handle screen, const char* path, f32 units);
// Whether a label is still revealing; finishes it at once.
bool ui_revealing(Engine& e, Handle screen, const char* path);
bool ui_reveal_all(Engine& e, Handle screen, const char* path);
// A node's rect after the last layout, in UI units from the screen's top-left.
rect2 ui_rect(Engine& e, Handle screen, const char* path);

// ---- the model -------------------------------------------------------------------------------
// Named values labels show as {key} and meters as "=key".
void ui_model_set_int(Engine& e, const char* key, i64 v);
void ui_model_set_float(Engine& e, const char* key, f64 v);
void ui_model_set_str(Engine& e, const char* key, const char* v);

struct UiStats { u32 screens, nodes, sprites, glyphs; f32 scale; };
const UiStats* ui_stats(Engine& e);

} // namespace kv
