// Korvid krome: the tree a screen is, and its layout. A screen is a tree of nodes of four
// kernel types (box, text, image, hit) with flags, properties and per-state looks; layout
// turns it into rects. Nothing here draws, reads input or knows Lua: it is plain data and
// two passes over it, so it is tested without a window (docs/PRDs/Krome-Slice-1-PRD.md,
// "The tree", "Layout").
//
//   KromeTree t;
//   t.init(mem_general());
//   u32 root = t.add(KromeType::Box);
//   t.set(root, "dir", KromeValue::str("col"));
//   t.set(root, "pad", KromeValue::num(12));
//   u32 label = t.add(KromeType::Text, root);
//   t.set_text(label, "Name");
//   krome_layout(t, root, rect2{0, 0, 640, 480}, measure);
//   t.nodes[label].rect  // where it went, in logical pixels
//
// Properties are set by name, as markup and Lua will name them; a property that does not
// apply, or a value of the wrong kind, is refused with the reason (krome_set returns it).
#pragma once
#include "korvid/core/array.h"
#include "korvid/core/name.h"
#include "korvid/math/color.h"
#include "korvid/math/rect2.h"

namespace kv {

constexpr u32 KROME_NONE      = 0xFFFFFFFFu;
constexpr u32 KROME_MAX_DEPTH = 64;

enum class KromeType  : u8 { Box, Text, Image, Hit, Edit };
enum class KromeDir   : u8 { Row, Col, Stack };
enum class KromeSize  : u8 { Hug, Fill, Fixed, Percent }; // Percent: `value` percent of the parent's inside
enum class KromeAlign : u8 { Start, Center, End };

// The states a look may differ in. Base is the node's own look.
enum class KromeState : u8 { Base, Hover, Pressed, Focused, Disabled, Selected, Count };

// One axis's size: hug the content, fill what the parent gives, or a number; any of them
// bounded by [lo, hi]. `set` says the mode was written (a bare `fill` flag fills the
// parent's direction only where the mode was not; a range alone sets no mode).
struct KromeAxis {
    KromeSize mode = KromeSize::Hug;
    f32       value = 0.0f;
    f32       lo = 0.0f, hi = __builtin_huge_valf();
    bool      set = false;
};

// What a node looks like. `set` has a bit per field written (KROME_LOOK_*), so a state's
// look overlays only what it says, and text color, font and size inherit when unset.
struct KromeLook {
    color bg{0, 0, 0, 0}, bg2{0, 0, 0, 0}; // bg2 set: a linear gradient from bg to bg2
    f32   angle = 90.0f;                   // the gradient's direction, degrees (90: top to bottom)
    f32   radius[4] = {};                  // top-left, top-right, bottom-right, bottom-left
    f32   border = 0.0f;
    color border_color{0, 0, 0, 0};
    vec2  shadow_offset;
    f32   shadow_blur = 0.0f, shadow_spread = 0.0f;
    color shadow_color{0, 0, 0, 0};
    color text_color{1, 1, 1, 1};
    f32   opacity = 1.0f;
    Name  font;                            // an asset path, interned
    f32   size = 0.0f;                     // font size, logical pixels
    Name  image;                           // an image node's texture, by asset path
    color tint{1, 1, 1, 1};
    f32   line_height = 0.0f;              // a text's lines, times its size; 0: the font's own
    color caret_color{0, 0, 0, 0};         // an edit's caret; unset: its text's color
    color selection_color{0.18f, 0.44f, 0.93f, 0.40f}; // what an edit's selection is drawn in
    color scrollbar_color{0.5f, 0.5f, 0.5f, 0.5f}; // a scroll box's thumb
    color scrollbar_track{0, 0, 0, 0};     // and the track under it
    f32   scrollbar_size = 6.0f;           // its width, logical pixels
    u32   set = 0;
};

constexpr u32 KROME_LOOK_BG       = 1u << 0;
constexpr u32 KROME_LOOK_BG2      = 1u << 1;
constexpr u32 KROME_LOOK_ANGLE    = 1u << 2;
constexpr u32 KROME_LOOK_RADIUS   = 1u << 3;
constexpr u32 KROME_LOOK_BORDER   = 1u << 4;
constexpr u32 KROME_LOOK_BORDER_COLOR = 1u << 5;
constexpr u32 KROME_LOOK_SHADOW   = 1u << 6;
constexpr u32 KROME_LOOK_SHADOW_COLOR = 1u << 7;
constexpr u32 KROME_LOOK_COLOR    = 1u << 8;
constexpr u32 KROME_LOOK_OPACITY  = 1u << 9;
constexpr u32 KROME_LOOK_FONT     = 1u << 10;
constexpr u32 KROME_LOOK_SIZE     = 1u << 11;
constexpr u32 KROME_LOOK_IMAGE    = 1u << 12;
constexpr u32 KROME_LOOK_TINT     = 1u << 13;
constexpr u32 KROME_LOOK_LINE_HEIGHT = 1u << 14;
constexpr u32 KROME_LOOK_CARET_COLOR = 1u << 15;
constexpr u32 KROME_LOOK_SELECTION_COLOR = 1u << 16;
constexpr u32 KROME_LOOK_SCROLLBAR_COLOR = 1u << 17;
constexpr u32 KROME_LOOK_SCROLLBAR_TRACK = 1u << 18;
constexpr u32 KROME_LOOK_SCROLLBAR_SIZE = 1u << 19;
constexpr u32 KROME_LOOK_INHERITED = KROME_LOOK_COLOR | KROME_LOOK_FONT | KROME_LOOK_SIZE | KROME_LOOK_LINE_HEIGHT;

// Node flags.
constexpr u32 KROME_FILL     = 1u << 0; // a bare `fill`: fill along the parent's direction
constexpr u32 KROME_CLIP     = 1u << 1; // children are clipped to this node's rect
constexpr u32 KROME_SCROLL   = 1u << 2; // clips, and moves its content along its direction
constexpr u32 KROME_DISABLED = 1u << 3;
constexpr u32 KROME_SELECTED = 1u << 4;
constexpr u32 KROME_HIDDEN   = 1u << 5; // takes no space and draws nothing
constexpr u32 KROME_DEAD     = 1u << 6; // rebuilt away: in no list, kept until the tree is built again
constexpr u32 KROME_SUBPIXEL = 1u << 7; // its text's pen is kept to a quarter pixel, not snapped to whole ones
constexpr u32 KROME_NOWRAP   = 1u << 8; // its text stays on one line whatever width it gets
constexpr u32 KROME_MULTILINE = 1u << 9; // an edit of many lines (Enter breaks one; Ctrl+Enter submits)
constexpr u32 KROME_READONLY  = 1u << 10; // an edit that selects and copies, and takes nothing
constexpr u32 KROME_SCROLL_X  = 1u << 11; // scroll=x (or xy): its content scrolls across
constexpr u32 KROME_SCROLL_Y  = 1u << 12; // scroll=y (or xy): and down
constexpr u32 KROME_FOCUSABLE = 1u << 13; // takes the keyboard focus though it has no event or behaviour
constexpr u32 KROME_NOFOCUS   = 1u << 14; // never takes it, though it has one
constexpr u32 KROME_SECRET    = 1u << 15; // an edit drawn as bullets, never copied
constexpr u32 KROME_TABS      = 1u << 16; // an edit that keeps Tab (a code editor's) rather than leaving on it

// Modifier keys, as a shortcut and a behaviour's key call see them.
constexpr u32 KROME_MOD_SHIFT = 1u << 0;
constexpr u32 KROME_MOD_CTRL  = 1u << 1;
constexpr u32 KROME_MOD_ALT   = 1u << 2; // Option on a Mac
constexpr u32 KROME_MOD_CMD   = 1u << 3; // Command on a Mac, the Windows key on a PC

// The pointer's shape over a node (`cursor`): unset is the arrow, an edit's the I-beam.
enum class KromeCursor : u8 { Unset, Arrow, IBeam, Hand, Move, ResizeEW, ResizeNS, Forbidden };

struct KromeNode {
    KromeType  type = KromeType::Box;
    KromeDir   dir = KromeDir::Col;
    KromeAlign align_x = KromeAlign::Start, align_y = KromeAlign::Start; // where children go
    i8         anchor_x = -1, anchor_y = -1; // in a stack: this node's own place (a KromeAlign), or -1 for the stack's
    u32        flags = 0;
    KromeAxis  w, h;
    f32        pad[4] = {}; // left, top, right, bottom
    f32        gap = 0.0f;
    u32        parent = KROME_NONE, first = KROME_NONE, last = KROME_NONE, next = KROME_NONE;
    u32        text = KROME_NONE;  // content: an offset into KromeTree::strings, as written
    u32        shown = KROME_NONE; // the content with {name} filled in, when someone filled it
    Name       id, event;
    Name       binding;        // @name: its selected state is the model's value there, a press flips it (an edit: its text)
    Name       change, blur;   // an edit's other events: its text changed, it lost the focus
    Name       context;        // fired by the right button, with where it was pressed
    u32        bind_value = KROME_NONE; // @key=value: the value, an offset into strings
    u32        tooltip = KROME_NONE;    // shown at the pointer after it rests here
    Name       behave;         // the behaviour the pointer and the keys go to (`behave=slider`)
    f32        lo = 0.0f, hi = 0.0f, step = 0.0f; // a behaviour's range (`slider @v 1..99`) and step; hi <= lo: none
    i16        taborder = 0;   // Tab goes through lower first, then document order
    u16        shortcut = 0;   // `key="mod+s"`: a Key (0 none), with shortcut_mods
    u8         shortcut_mods = 0;
    u32        placeholder = KROME_NONE; // an edit's, shown while it is empty: an offset into strings
    KromeCursor cursor = KromeCursor::Unset;
    u32        look = 0;           // the base look: an index into KromeTree::looks
    u32        states[(u32)KromeState::Count] = {}; // for each state, 1 + an index into looks; 0: none
    u32        line = 0;           // where it was written, for errors (0: unknown)

    // Layout's answers, in logical pixels.
    vec2  want;       // what it asked for
    f32   narrowest = 0.0f; // the least width it can take: a wrapping text's longest word
    rect2 rect;       // what it got, in the screen's space
    vec2  content;    // a container's children's extent (what a scroll box scrolls over)
    vec2  scroll;     // a scroll box's offset, clamped by layout
};

// A value for a property: a number, a string, a range, or up to four numbers.
struct KromeValue {
    enum Kind : u8 { Number, String, Range, List } kind = Number;
    f32         n = 0.0f;
    f32         lo = 0.0f, hi = 0.0f;
    const char* s = nullptr;
    f32         list[4] = {};
    u32         count = 0;

    static KromeValue num(f32 v) { KromeValue x; x.kind = Number; x.n = v; return x; }
    static KromeValue str(const char* v) { KromeValue x; x.kind = String; x.s = v; return x; }
    static KromeValue range(f32 a, f32 b) { KromeValue x; x.kind = Range; x.lo = a; x.hi = b; return x; }
    static KromeValue nums(const f32* v, u32 n) {
        KromeValue x;
        x.kind = List;
        x.count = n > 4 ? 4 : n;
        for (u32 i = 0; i < x.count; ++i) x.list[i] = v[i];
        return x;
    }
};

// Which ways a node's content scrolls: `scroll` alone is along its direction, `scroll=x|y|xy`
// says which.
inline bool krome_scrolls_x(const KromeNode& n) { return (n.flags & KROME_SCROLL_X) || ((n.flags & KROME_SCROLL) && n.dir == KromeDir::Row); }
inline bool krome_scrolls_y(const KromeNode& n) { return (n.flags & KROME_SCROLL_Y) || ((n.flags & KROME_SCROLL) && n.dir != KromeDir::Row); }

// A shortcut as `key=` writes it ("mod+s", "ctrl+shift+z", "f5"; `mod` is Command on a Mac,
// else Ctrl): a Key and KROME_MOD_ bits. False when it is not one.
bool krome_parse_shortcut(const char* text, u32* key, u32* mods);

struct KromeTree {
    Array<KromeNode> nodes;
    Array<KromeLook> looks;   // looks[0] is the empty look
    Array<char>      strings; // NUL-terminated, by offset
    u32              root = KROME_NONE;
    bool             dirty = true; // something layout reads has changed since the last layout

    void init(Allocator* a);
    void free();
    void clear();

    // A new node, last child of `parent` (or a root when KROME_NONE; the first root is
    // the tree's). KROME_NONE past KROME_MAX_DEPTH.
    u32  add(KromeType type, u32 parent = KROME_NONE);
    // Sets a property in a state's look (Base: the node's own, layout properties too).
    // Null when it took; otherwise why not, a static string.
    const char* set(u32 node, const char* prop, const KromeValue& v, KromeState state = KromeState::Base);
    // A bare word: fill, clip, scroll, disabled, selected, hidden, and the alignments
    // left, center, right, top, middle, bottom. Null when it took.
    const char* flag(u32 node, const char* word);
    void set_text(u32 node, const char* text, u32 len);
    void set_text(u32 node, const char* text);
    void set_shown(u32 node, const char* text, u32 len); // the content as displayed
    const char* text(u32 node) const;  // the content as displayed (shown, else as written); "" if none
    const char* raw_text(u32 node) const; // as written

    // The look a node has in a state: its base, the state's look over it, and what
    // inherits (text color, font, size) taken from `parent` (the parent's look as already
    // resolved, in whatever state the parent was) where the node does not say. Null
    // parent: the defaults.
    KromeLook resolve_look(u32 node, KromeState state, const KromeLook* parent) const;

    u32 string(const char* s, u32 len); // stores a copy, returns its offset
};

// How layout asks the size of content it cannot see: text in a font, and an image.
struct KromeMeasure {
    void* user = nullptr;
    // The text's extent in logical pixels at `size`, one line.
    vec2 (*text)(void* user, const char* s, u32 len, Name font, f32 size) = nullptr;
    // An image's natural size in logical pixels; zero if not known yet.
    vec2 (*image)(void* user, Name path) = nullptr;
    // A paragraph's: its widest line and its height broken at `width` (<= 0: only at `\n`),
    // its lines `line_height` times its size apart (0: the font's), and the narrowest width it
    // can break to. Without it, text is one line and does not wrap.
    vec2 (*paragraph)(void* user, const char* s, u32 len, Name font, f32 size, f32 line_height, f32 width, f32* narrowest) = nullptr;
};

// The default font and size where nothing says (DejaVu Sans, which the text module ships).
extern const char* const KROME_DEFAULT_FONT;
constexpr f32 KROME_DEFAULT_SIZE = 14.0f;

// Lays out the subtree at `node` in `space`: the node takes the whole of it when `fill`
// (a screen's root, the window), else the size it wants, placed at space's corner (an
// overlay, which its caller then moves). Clears tree.dirty when node is the root.
void krome_layout(KromeTree& t, u32 node, rect2 space, const KromeMeasure& m, bool fill = true);

// A logical rect in physical pixels at `scale`, each edge rounded to the nearest pixel, so
// rects that share an edge still share it.
rect2 krome_pixel_rect(rect2 r, f32 scale);

// Parses "#rgb", "#rgba", "#rrggbb", "#rrggbbaa" and "none" (sRGB, 0 to 1). False for
// anything else.
bool krome_parse_color(const char* s, color* out);

const char* krome_type_name(KromeType t);
const char* krome_state_name(KromeState s);
bool        krome_state_of(const char* name, KromeState* out);

} // namespace kv
