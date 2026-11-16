// Korvid patherizer: paths, filled and stroked, rasterized on the CPU at the size they are drawn
// (docs/PRDs/Krome-Slice-5-PRD.md). A shape is plain data: groups over paths, paths of
// lines and curves with a fill and a stroke, paints solid, `currentColor` or gradients. It
// draws into a caller's RGBA buffer, the same bytes on every platform: coverage is integer
// arithmetic, and the float work (flattening, transforms, gradients) is the engine's math.
//
//   PathShape s;
//   s.init(mem_general());
//   PathBuilder b(s);
//   b.move_to(2, 2); b.line_to(14, 2); b.line_to(8, 14); b.close();
//   b.fill(s.add_solid(255, 128, 0, 255));
//   patherizer_draw(s, target, PathOptions{});
//
// Nothing here knows Krome; the svg module builds shapes from SVG, and the asset module reads
// them from .kshape files.
#pragma once
#include "korvid/core/array.h"
#include "korvid/core/types.h"

namespace kv {

struct Allocator;

enum class PathVerb : u8 { Move, Line, Quad, Cubic, Close };
enum class FillRule : u8 { NonZero, EvenOdd };
enum class GradientSpread : u8 { Pad, Reflect, Repeat };
enum class LineJoin : u8 { Miter, Round, Bevel };
enum class LineCap : u8 { Butt, Round, Square };

// An affine transform: x' = a x + c y + e, y' = b x + d y + f (SVG's matrix(a b c d e f)).
struct PathTransform {
    f32 m[6] = {1, 0, 0, 1, 0, 0};
    static PathTransform translate(f32 x, f32 y) { PathTransform t; t.m[4] = x; t.m[5] = y; return t; }
    static PathTransform scale(f32 sx, f32 sy) { PathTransform t; t.m[0] = sx; t.m[3] = sy; return t; }
    // this after `first`: a point goes through `first`, then this.
    PathTransform then(const PathTransform& after) const;
    void apply(f32 x, f32 y, f32* ox, f32* oy) const { *ox = m[0] * x + m[2] * y + m[4]; *oy = m[1] * x + m[3] * y + m[5]; }
    bool invert(PathTransform* out) const;
};

struct PathStop {
    f32 offset;  // 0 to 1, ascending
    u8  rgba[4]; // sRGB, straight alpha
};

// What a fill or a stroke paints with. A gradient's geometry is in its own space, which
// `transform` takes to the shape's user space (an objectBoundingBox gradient is converted
// to user space when the shape is built).
struct PathPaint {
    enum Kind : u8 { None, Solid, Current, Linear, Radial };
    Kind           kind = None;
    u8             rgba[4] = {0, 0, 0, 255};   // Solid
    GradientSpread spread = GradientSpread::Pad;
    bool           linear_rgb = false;         // stops interpolate in linear light (color-interpolation: linearRGB)
    f32            g[5] = {};                  // Linear: x1 y1 x2 y2; Radial: cx cy r fx fy
    PathTransform  transform;
    u32            first_stop = 0, stop_count = 0;
};

struct PathStroke {
    f32      width = 1.0f;
    LineJoin join = LineJoin::Miter;
    LineCap  cap = LineCap::Butt;
    f32      miter_limit = 4.0f;
    u32      first_dash = 0, dash_count = 0; // lengths, alternately on and off
    f32      dash_offset = 0.0f;
};

// A node of the shape's tree. A group's children are the nodes after it, `count` of them
// (its whole subtree); a path's commands are `verb_count` verbs from `first_verb`, their
// points from `first_point`.
struct PathNode {
    enum Kind : u8 { Path, Group };
    Kind          kind = Path;
    FillRule      rule = FillRule::NonZero;
    PathTransform transform;           // the node's own, after its parents'
    // A path:
    u32 first_verb = 0, verb_count = 0, first_point = 0;
    u32 fill = 0, stroke = 0;          // paint indices; 0 is None
    f32 fill_opacity = 1.0f, stroke_opacity = 1.0f;
    u32 stroke_style = 0;              // index into strokes (with a stroke paint)
    // A group:
    u32 count = 0;                     // the nodes in its subtree, after it
    f32 opacity = 1.0f;
    u32 clip = 0;                      // 1 + the node index of a clip group; 0 none
    u32 line = 0;                      // the source's line (SVG), for messages; 0 none
};

struct PathShape {
    f32              view[4] = {0, 0, 0, 0}; // x, y, w, h: what a target's rectangle shows
    Array<PathVerb>  verbs;
    Array<f32>       points;  // x, y pairs, in user space
    Array<PathPaint> paints;  // [0] is None
    Array<PathStop>  stops;
    Array<PathStroke> strokes;
    Array<f32>       dashes;
    Array<PathNode>  nodes;   // the tree in order; the roots are drawn in order
    Array<u32>       clips;   // the clip groups' node indices (drawn only as clips)

    void init(Allocator* a);
    void free();
    void clear();
    u32  add_solid(u8 r, u8 g, u8 b, u8 a);
    u32  add_current();       // currentColor
    u32  add_gradient(const PathPaint& p, const PathStop* stops, u32 count);
    // True when every paint any node uses is currentColor (or None): a one-colour shape,
    // which a caller may rasterize once in white and tint.
    bool one_colour() const;
};

// Builds a shape's paths in order: commands, then fill() or stroke() ends the path.
struct PathBuilder {
    PathShape& s;
    u32        verb0, point0;
    f32        cx = 0, cy = 0, sx = 0, sy = 0; // the current point and the subpath's start
    PathTransform transform;                    // given to the paths that follow
    explicit PathBuilder(PathShape& shape);
    void move_to(f32 x, f32 y);
    void line_to(f32 x, f32 y);
    void quad_to(f32 x1, f32 y1, f32 x, f32 y);
    void cubic_to(f32 x1, f32 y1, f32 x2, f32 y2, f32 x, f32 y);
    void close();
    // Ends the path: its commands so far filled, stroked, or both. Returns its node.
    u32  fill(u32 paint, FillRule rule = FillRule::NonZero, f32 opacity = 1.0f);
    u32  stroke(u32 paint, const PathStroke& style, f32 opacity = 1.0f, const f32* dashes = nullptr, u32 dash_count = 0);
    u32  fill_and_stroke(u32 fill_paint, FillRule rule, u32 stroke_paint, const PathStroke& style);
    // Groups: begin, the children, end.
    u32  begin_group(f32 opacity = 1.0f, u32 clip = 0);
    void end_group(u32 group);
    // SVG path data (the `d` attribute): every command, relative and absolute, arcs as
    // cubics. False, with the byte offset in *at, when it cannot be read.
    bool svg_path(const char* d, u32* at = nullptr);
private:
    u32 end_path();
};

// Where a shape draws: RGBA8, rows `pitch` bytes apart.
struct PathTarget {
    u8*  pixels = nullptr;
    u32  width = 0, height = 0, pitch = 0;
    bool premultiplied = false; // the output's alpha: straight (what the UI primitive reads) or premultiplied
};

struct PathOptions {
    // The shape's view box fills the target, scaled the same each way and centred (SVG's
    // xMidYMid meet); `stretch` scales each way to fill.
    bool stretch = false;
    u8   current[4] = {255, 255, 255, 255}; // currentColor, sRGB, straight
    Allocator* scratch = nullptr;           // the coverage and the layers; null: the general allocator
};

// Draws the shape over the target's pixels (clear the target first for a picture alone).
// False when it cannot (no memory).
bool patherizer_draw(const PathShape& shape, const PathTarget& target, const PathOptions& options);

} // namespace kv
