// Korvid ui: the module's state, shared by the documents, the layout and the systems.
#pragma once
#include "korvid/ui/module.h"
#include "korvid/ui/ui.h"
#include "korvid/core/array.h"
#include "korvid/core/json.h"
#include "korvid/core/map.h"
#include "korvid/core/pool.h"
#include "korvid/asset/texture.h"

namespace kv {

struct TickContext;
struct FontAsset;

constexpr u32 KV_UI_STYLE_ENTRIES = 64;
constexpr u32 KV_UI_PATH_MAX      = KV_ASSET_PATH_MAX;

// ---- the style ------------------------------------------------------------------------------

struct UiFrame { Name name; Handle texture; Handle material; f32 insets[4]; };          // left, top, right, bottom (px)
struct UiImage { Name name; Handle texture; Handle material; rect2 rect; u32 frames; f32 scale; }; // a strip of `frames` rects, left to right, drawn at `scale` units a pixel
struct UiColor { Name name; color c; };
struct UiSound { Name name; Handle sound; };
struct UiFont  { u32 px; Handle font; };                                                 // the style's font at one pixel size

struct UiDoc {
    char        path[KV_UI_PATH_MAX];
    AssetState  state = AssetState::Pending;
    AssetError  error = AssetError::None;
    u32         refs  = 0;
    Handle      blob;                 // the file: kept, since a screen opens from its JSON
    bool        parsed = false;
    vec2        design{480, 270};
    bool        pixel = true;
    char        font_path[KV_UI_PATH_MAX] = {};
    f32         font_px = 16.0f;
    f32         line = 0.0f;          // line height in units; 0: the font's
    f32         space = 0.0f;         // a space's width in units; 0: the font's (some pixel fonts have none worth the name)
    color       shadow{0, 0, 0, 0};
    char        actions[3][24];       // move, confirm, cancel
    Array<UiFrame> frames;
    Array<UiImage> images;
    Array<UiColor> colors;
    Array<UiSound> sounds;
    Array<UiFont>  fonts;             // by pixel size, made as the window asks
    char        error_text[160] = {};
};

// ---- screens ---------------------------------------------------------------------------------

struct UiNode {
    Name    name;
    UiType  type;
    u32     parent = KV_UI_NONE, first = KV_UI_NONE, last = KV_UI_NONE, next = KV_UI_NONE;
    UiPoint anchor, pivot;
    vec2    offset;
    UiSize  w_mode, h_mode;
    vec2    size;
    f32     padding[4];
    f32     gap;
    UiDir   dir;
    UiAlign align;
    bool    visible, wrap, bar, input;
    char*   text = nullptr;          // owned: the module's allocator
    u32     text_cap = 0;
    Name    color, shadow, frame, fill, image, event;
    f32     font_scale;
    f32     reveal_rate;
    u32     frame_index;
    char    value[24], max[24];      // a number or "=key"
    u32     max_length;
    // A texture set by code, in place of a style image.
    Handle  texture, material;
    rect2   tex_rect;
    // State.
    f32     reveal_t = 0.0f;         // seconds into the reveal
    u32     reveal_shown = 0;        // characters shown at the last update
    bool    revealed = true;         // the reveal has finished (and said so)
    u32     focus = 0;               // list: the focused item's index among the children
    f32     scroll = 0.0f;           // scroll: units scrolled down
    // Layout, in units from the screen's top-left.
    f32     x = 0, y = 0, w = 0, h = 0;
    rect2   clip;                    // the visible rect (scroll ancestors), units
};

struct UiScreen {
    char         name[32];
    Handle       doc;
    UiOpen       open;
    Array<UiNode> nodes;             // 0 is the root
    bool         built = false;      // the document's nodes are in (a code screen: from the start)
    bool         code  = false;
    u32          order = 0;          // opening order, for the topmost
    f32          time  = 0.0f;       // seconds open, every tick (effects)
    // Coming and going: t runs 0 to 1 over the motion's seconds; leaving closes at 1.
    UiMotion     motion;
    f32          motion_t = 1.0f;
    bool         leaving  = false;
};

struct UiCoverState {
    u32     id = 0;
    UiCover desc;
    f32     amount = 0.0f;           // 0 open, 1 covered
    u8      phase  = 0;              // 0 closing, 1 covered (waiting), 2 opening
    bool    wait   = false;
    bool    said   = false;          // UiCovered went out
};

struct UiModelValue { u8 type; i64 i; f64 f; Name s; }; // 0 int, 1 float, 2 string

// One quad of output.
struct UiQuad {
    Handle material;
    rect2  uv;
    f32    x, y, w, h;               // window pixels, top-left origin, y down
    color  tint;
    i32    layer;
};

struct UiState_ {
    Allocator*          general = nullptr;
    Pool<UiDoc>         docs;
    Map<Name, Handle>   doc_by_path;
    Pool<UiScreen>      screens;
    Map<u64, UiModelValue> model;
    Array<UiQuad>       quads;       // this tick's output
    Array<Handle>       rows;        // the ui.sprite rows, reused in order
    u16                 doc_kind = 0, screen_kind = 0;
    u32                 sprite_kind = 0;
    u32                 next_order = 1;
    Handle              white_texture, white_material;
    f32                 scale = 1.0f;// pixels per unit, this tick
    f32                 win_w = 1280.0f, win_h = 720.0f;
    bool                holding = false;
    Array<UiCoverState> covers;      // at most a few
    u32                 next_cover = 1;
    Handle              iris_texture, iris_material; // a disc of clear in black, for the iris
    bool                text_on = false; // a field holds the platform's text input
    u64                 keys_prev[2] = {};// last tick's Enter and Escape, for edges
    UiStats             stats;
};

UiState_* ui_state(Engine& e);

// doc.cc
bool        ui_docs_init(Engine& e, UiState_& s, Allocator* general, MemTag tag);
void        ui_docs_shutdown(Engine& e, UiState_& s);
void        ui_docs_pump(Engine& e, UiState_& s);
const UiFrame* ui_doc_frame(const UiDoc& d, Name n);
const UiImage* ui_doc_image(const UiDoc& d, Name n);
const UiSound* ui_doc_sound(const UiDoc& d, const char* name);
bool        ui_doc_color(const UiDoc& d, Name n, color* out);
bool        ui_color_parse(const UiDoc* d, const char* text, color* out); // a style name or #rrggbb[aa]
const FontAsset* ui_doc_font(Engine& e, UiState_& s, UiDoc& d, u32 px);
// The screen's nodes from the document's JSON. False with a log line when it has none.
bool        ui_build_from_doc(Engine& e, UiState_& s, UiScreen& sc);
// A node from a descriptor; its strings copied.
u32         ui_node_add(UiState_& s, UiScreen& sc, u32 parent, const UiNodeDesc& d);
void        ui_node_free(UiState_& s, UiNode& n);
void        ui_node_set_text(UiState_& s, UiNode& n, const char* text);

// layout.cc
void        ui_layout(Engine& e, UiState_& s, UiScreen& sc);
void        ui_emit(Engine& e, UiState_& s, UiScreen& sc);
// The rich text's characters: how many there are to reveal, and how long they take.
u32         ui_text_chars(UiState_& s, const char* text, f32 rate, f32* seconds);
u32         ui_text_shown(UiState_& s, const char* text, f32 rate, f32 t);
// A model value or a number, as text into `out`.
void        ui_model_text(UiState_& s, const char* key, char* out, usize cap);
f32         ui_number(UiState_& s, const char* value_or_key, f32 dflt);

} // namespace kv
