// Korvid sprite_ui: the module's state, shared by the documents, the layout and the systems.
#pragma once
#include "korvid/sprite_ui/module.h"
#include "korvid/sprite_ui/sprite_ui.h"
#include "korvid/core/array.h"
#include "korvid/core/json.h"
#include "korvid/core/map.h"
#include "korvid/core/pool.h"
#include "korvid/asset_render/texture.h"

namespace kv {

struct TickContext;
struct FontAsset;

constexpr u32 KV_SPUI_STYLE_ENTRIES = 64;
constexpr u32 KV_SPUI_PATH_MAX      = KV_ASSET_PATH_MAX;

// ---- the style ------------------------------------------------------------------------------

struct SpuiFrame { Name name; Handle texture; Handle material; f32 insets[4]; };          // left, top, right, bottom (px)
struct SpuiImage { Name name; Handle texture; Handle material; rect2 rect; u32 frames; f32 scale; }; // a strip of `frames` rects, left to right, drawn at `scale` units a pixel
struct SpuiColor { Name name; color c; };
struct SpuiSound { Name name; Handle sound; };
struct SpuiFont  { u32 px; Handle font; };                                                 // the style's font at one pixel size

struct SpuiDoc {
    char        path[KV_SPUI_PATH_MAX];
    AssetState  state = AssetState::Pending;
    AssetError  error = AssetError::None;
    u32         refs  = 0;
    Handle      blob;                 // the file: kept, since a screen opens from its JSON
    bool        parsed = false;
    vec2        design{480, 270};
    bool        pixel = true;
    char        font_path[KV_SPUI_PATH_MAX] = {};
    f32         font_px = 16.0f;
    f32         line = 0.0f;          // line height in units; 0: the font's
    f32         space = 0.0f;         // a space's width in units; 0: the font's (some pixel fonts have none worth the name)
    color       shadow{0, 0, 0, 0};
    u32         shadow_offset = 1;    // units down and right (times a label's font_scale); drawn at every unit out to it, a solid drop
    char        actions[3][24];       // move, confirm, cancel
    Array<SpuiFrame> frames;
    Array<SpuiImage> images;
    Array<SpuiColor> colors;
    Array<SpuiSound> sounds;
    Array<SpuiFont>  fonts;             // by pixel size, made as the window asks
    char        error_text[160] = {};
};

// ---- screens ---------------------------------------------------------------------------------

struct SpuiNode {
    Name    name;
    SpuiType  type;
    u32     parent = KV_SPUI_NONE, first = KV_SPUI_NONE, last = KV_SPUI_NONE, next = KV_SPUI_NONE;
    SpuiPoint anchor, pivot;
    vec2    offset;
    SpuiSize  w_mode, h_mode;
    vec2    size;
    f32     padding[4];
    f32     gap;
    SpuiDir   dir;
    SpuiAlign align;
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

struct SpuiScreen {
    char         name[32];
    Handle       doc;
    SpuiOpen       open;
    Array<SpuiNode> nodes;             // 0 is the root
    bool         built = false;      // the document's nodes are in (a code screen: from the start)
    bool         code  = false;
    u32          order = 0;          // opening order, for the topmost
    f32          time  = 0.0f;       // seconds open, every tick (effects)
    // Coming and going: t runs 0 to 1 over the motion's seconds; leaving closes at 1.
    SpuiMotion     motion;
    f32          motion_t = 1.0f;
    bool         leaving  = false;
};

struct SpuiCoverState {
    u32     id = 0;
    SpuiCover desc;
    f32     amount = 0.0f;           // 0 open, 1 covered
    u8      phase  = 0;              // 0 closing, 1 covered (waiting), 2 opening
    bool    wait   = false;
    bool    said   = false;          // SpuiCovered went out
};

struct SpuiModelValue { u8 type; i64 i; f64 f; Name s; }; // 0 int, 1 float, 2 string

// One quad of output.
struct SpuiQuad {
    Handle material;
    rect2  uv;
    f32    x, y, w, h;               // window pixels, top-left origin, y down
    color  tint;
    i32    layer;
};

struct SpuiState_ {
    Allocator*          general = nullptr;
    Pool<SpuiDoc>         docs;
    Map<Name, Handle>   doc_by_path;
    Pool<SpuiScreen>      screens;
    Map<u64, SpuiModelValue> model;
    Array<SpuiQuad>       quads;       // this tick's output
    Array<Handle>       rows;        // the sprite_ui.sprite rows, reused in order
    u16                 doc_kind = 0, screen_kind = 0;
    u32                 sprite_ktable = 0;
    u32                 next_order = 1;
    Handle              white_texture, white_material;
    f32                 scale = 1.0f;// pixels per unit, this tick
    f32                 win_w = 1280.0f, win_h = 720.0f;
    bool                holding = false;
    Array<SpuiCoverState> covers;      // at most a few
    u32                 next_cover = 1;
    Handle              iris_texture, iris_material; // a disc of clear in black, for the iris
    bool                text_on = false; // a field holds the platform's text input
    u64                 keys_prev[2] = {};// last tick's Enter and Escape, for edges
    SpuiStats             stats;
};

SpuiState_* spui_state(Engine& e);

// doc.cc
bool        spui_docs_init(Engine& e, SpuiState_& s, Allocator* general, MemTag tag);
void        spui_docs_shutdown(Engine& e, SpuiState_& s);
void        spui_docs_pump(Engine& e, SpuiState_& s);
const SpuiFrame* spui_doc_frame(const SpuiDoc& d, Name n);
const SpuiImage* spui_doc_image(const SpuiDoc& d, Name n);
const SpuiSound* spui_doc_sound(const SpuiDoc& d, const char* name);
bool        spui_doc_color(const SpuiDoc& d, Name n, color* out);
bool        spui_color_parse(const SpuiDoc* d, const char* text, color* out); // a style name or #rrggbb[aa]
const FontAsset* spui_doc_font(Engine& e, SpuiState_& s, SpuiDoc& d, u32 px);
// The screen's nodes from the document's JSON. False with a log line when it has none.
bool        spui_build_from_doc(Engine& e, SpuiState_& s, SpuiScreen& sc);
// A node from a descriptor; its strings copied.
u32         spui_node_add(SpuiState_& s, SpuiScreen& sc, u32 parent, const SpuiNodeDesc& d);
void        spui_node_free(SpuiState_& s, SpuiNode& n);
void        spui_node_set_text(SpuiState_& s, SpuiNode& n, const char* text);

// layout.cc
void        spui_layout(Engine& e, SpuiState_& s, SpuiScreen& sc);
void        spui_emit(Engine& e, SpuiState_& s, SpuiScreen& sc);
// The rich text's characters: how many there are to reveal, and how long they take.
u32         spui_text_chars(SpuiState_& s, const char* text, f32 rate, f32* seconds);
u32         spui_text_shown(SpuiState_& s, const char* text, f32 rate, f32 t);
// A model value or a number, as text into `out`.
void        spui_model_text(SpuiState_& s, const char* key, char* out, usize cap);
f32         spui_number(SpuiState_& s, const char* value_or_key, f32 dflt);

} // namespace kv
