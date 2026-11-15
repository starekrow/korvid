# Krome Slice 3 — Text

Nov 15, 2026 · @starekrow · draft

## Goal and exit criteria

Slice 3 makes text good enough to build tools out of: paragraphs that wrap, glyphs made
as they are needed at the exact size they are drawn, small text that is crisp, spacing
that is even when asked for, and `edit`, the kernel's fifth node, which takes typed,
pasted and composed text. The master PRD (`Krome-Master-PRD.md`, "Text" and "Native text
and accessibility") says what each is for; slices 1 and 2 built the kernel and the markup
this stands on. This says what is built now, in what order, and how we know it worked.

Most of it is not Krome's. Paragraph layout and the glyph cache serve a game's labels and
`ui` as much as Krome, and typed text, composition, the clipboard and the cursor's shape
are the platform's. Krome is the first customer of each.

It is done when:

- **A multi-line `edit`** in kromedemo takes typed text, text pasted from another program,
  and text composed through an input method (Linux with IBus or Fcitx, and macOS), with a
  caret, selection by pointer and keys, cut, copy and paste through the system clipboard,
  and undo and redo of typing. What it holds is the model's (`edit @notes`), both ways.
- **A game label wraps** at a width: `Text` with a wrap width lays out over several lines,
  aligned, in an example (rocks' pause screen, its hint wrapped to a width, centered) and
  in `text_tests`.
- **Krome's `text` wraps** at the width layout gives it, and a column of wrapped
  paragraphs lays out at its true height (layout goes width first, then height).
- **A 9 px label is as crisp as the platform's own**: kromedemo's `--shots` at 9, 11 and
  13 px beside the same strings drawn by the desktop (a screenshot of a GTK label, a
  macOS one), judged by eye, and stems and baselines on whole pixels by measurement.
- **Glyphs on demand.** A screen with text outside today's fixed set (Greek, Cyrillic,
  box drawing, a CJK line through a fallback font) draws it without declaring a range;
  a window moved to a display of another scale draws at the new exact size; the atlas
  grows in pages, and a run that draws only ASCII rasterizes only what it drew.
- **Subpixel positioning, asked for** (`subpixel` on a node, `Text::subpixel` on a game
  label), spaces 9 to 11 px text evenly where whole-pixel pens do not, in a picture.
- **The cursor** is an I-beam over an `edit`, a hand over what says `cursor=hand`, and
  an arrow elsewhere.
- **Headless tests** cover line breaking, alignment, caret and selection geometry, the
  edit buffer's operations and its undo, and the glyph cache's pages (`text_tests`,
  `krome_tests`, `asset_tests`); `render-gate` agrees on Linux.

## What the code has

- **Fonts** (`korvid/asset/font.h`): a `.ttf` read through the I/O queue and rasterized
  with stb_truetype at one pixel size into one atlas of a fixed set (about 360 glyphs:
  ASCII, Latin-1, Latin Extended-A, punctuation, U+FFFD), uploaded whole, RGBA8 white with
  coverage in alpha. Kerning pairs. A code point outside the set draws U+FFFD. No hinting,
  no fallback, no growth.
- **The renderer** uploads a texture whole (`renderer_texture_upload`); there is no
  update of part of one.
- **`text`** (`modules/text`): `Text { font, str[64], scale, tint, ... }`, one line, laid
  out as glyph sprites in the Frame cycle; `text_layout` and `font_measure` for one line.
- **`ui`** wraps its labels itself (`lay_text`, word by word at a width), at pixel sizes
  of its own.
- **Krome** draws `text` as one line, a glyph per code point, from the asset font at the
  size times the window's scale (`font_for`). Layout measures text as one line, and goes
  up for what each node wants and down for what it gets, so a node's height cannot
  depend on its width.
- **Typed text** (`platform_hid`): UTF-8 the OS typed (layouts, dead keys applied) is
  collected into `InputState::text` (64 bytes a tick) while `platform_text_input` is on,
  with the Backspaces among it. SDL3 (Linux, macOS) starts and stops the OS's text input;
  Win32 takes `WM_CHAR`; Android has `commitText` and `setComposingText`. No composition
  reaches the engine, there is no clipboard, and there is no cursor shape.
- **Keys** arrive as `KeyDown` and `KeyUp` events with repeats; Krome reads Escape.

## The shape

### Where things are

| path | what |
|---|---|
| `modules/asset/` | the glyph cache: fonts that rasterize what is asked, at any size, into pages; a fallback list; hinting by snapping; subpixel offsets |
| `modules/render/` + backends | `renderer_texture_update`: part of a texture, so a page takes a new glyph without uploading the page |
| `modules/typeset/` (new) | paragraphs: breaking, alignment, line height, measure; caret, hit and selection geometry; the edit buffer and its undo. Core, math and asset only |
| `modules/text/` | `Text` wraps at `wrap`; long strings (open, below) |
| `modules/ui/` | its labels on `typeset` (P2) |
| `modules/platform_hid/` | `TextTyped`, `TextComposing`; the input method's area; the clipboard; the cursor's shape |
| `modules/krome/` | `text` wraps; layout width then height; `edit`; focus by pointer; caret, selection and composition drawn |
| `modules/kimb/` | `edit` and the new properties in the kernel's vocabulary |
| `modules/krome/assets/std.kim` | `input` and `textarea` over `edit`; `cursor=hand` on buttons |

### Glyphs on demand

A font asset stops being one atlas of a fixed set. It becomes a face (the file, parsed
once) and, for each pixel size asked for, a strike: a table of the glyphs rasterized so far
and the pages they are in.

- **Asked, then made.** `font_glyph(font, px, cp)` returns the glyph if the strike has it;
  otherwise it is queued and returned as made at the end of the frame (the pump rasterizes
  what was queued, at most a budget's worth a frame), and the text that asked lays out
  again when it lands. A glyph is rasterized once per strike.
- **Pages.** 512 by 512, R8 coverage (a quarter of today's RGBA8), packed by shelves; a
  strike takes another page when one fills. A page changed in a frame is updated by the
  rectangles that changed (`renderer_texture_update`).
- **Exact sizes.** A strike is a whole pixel size: Krome asks for the size times the
  window's scale, rounded, so text at 1.5x is rasterized at 1.5x, not scaled.
- **Fallback.** A font asset names a list of faces (`FontDesc::fallback`); a code point
  the first does not have is looked for down the list, and drawn from the one that has it,
  in that face's metrics on the first face's baseline. U+FFFD only when none has it.
- **Crisp small text.** stb_truetype does not hint. What it does instead: the baseline,
  the pen (unless subpixel) and the glyph's box on whole pixels; vertical metrics rounded
  so x-height lands on a pixel edge where the face allows; and a contrast curve on the
  coverage below 14 px (a stem that would be two half-dark pixels reads as one darker
  one). Whether that reaches the done-when is the slice's first question (below).
- **Subpixel positioning.** A strike asked for subpixel keeps up to four copies of a glyph
  at quarter-pixel pen offsets, made as they are asked for; the pen is kept fractional and
  the copy nearest its fraction drawn.

### Paragraphs: `typeset`

One layout of text for everyone, depending on `core`, `math` and `asset` only, so Krome,
`text` and `ui` each link it without bringing the others.

- **Breaking.** At spaces and after hyphens and dashes; a word too long for its line breaks
  inside it, between code points. `\n` is a hard break. Trailing spaces hang past the
  edge. No hyphenation, no shaping beyond kerning, left to right only.
- **A paragraph** (`typeset_paragraph(font, px, text, width, align, line_height, out)`):
  its lines (byte ranges, widths, baselines) and its glyphs' positions; its measure without
  the glyphs (`typeset_measure`: the widest line, the height, and the narrowest width it
  can take, its longest word).
- **Geometry for an editor.** The caret's rectangle at a byte offset; the offset nearest a
  point (by half a glyph); the rectangles a selection covers; the offsets one line up or
  down at the same x, a word left or right, the line's start and end.
- **The edit buffer.** UTF-8 text with a caret and a selection (byte offsets on code point
  boundaries), and the operations an editor does: insert, delete back and forward (by
  code point or word), move (by code point, word, line, page, document), extend the
  selection, select a word or a line. Undo and redo: typing is grouped until the caret
  moves, a pause, or a space after a word; a paste, a cut and a deletion of a selection are
  steps of their own. Pure data, tested headless.

### Krome

- **`text` wraps.** Its width is what layout gives it; when that is less than its one-line
  width, it wraps. `nowrap` keeps it on one line (clipped by whatever clips). Its own
  `left`, `center` and `right` align its lines. `line-height` (a multiple of the size,
  1.25 by default) is a look property and inherits.
- **Layout goes width, then height.** Up for each node's width (its widest and its
  narrowest: a paragraph's longest word), down to give widths; then up for heights at
  those widths, down to place. Rows and columns are as they were otherwise; a screen
  with no wrapped text lays out to the same rects (slice 1's tests hold).
- **`edit`**, the fifth node. A box that holds text the person types: one line, or many
  with `multiline`; `readonly`; `placeholder="..."` shown while it is empty; `@name` binds
  what it holds to the model, written back as it changes. Three events: `-> event` on
  Enter (one line) or Ctrl+Enter (many); `change=event` when what it holds changed (at
  most once a frame, however many keys the frame had); `blur=event` when it loses focus.
  Each reaches its Lua handler and C++ as a `KromeEvent`; the text is the model's, so a
  handler reads it there. It clips its text and scrolls to keep the caret in view.
  Its look: the text's (font, size, color), plus `caret-color` and `selection-color`.
- **Focus, by pointer.** A press on an `edit` focuses it (the `focused` state, already in
  every look); a press elsewhere, Escape, or the window losing focus blurs it. One `edit`
  has focus at a time across Krome's screens. Tab order, shortcuts and focus without a
  pointer are slice 4's.
- **Keys.** With focus, Krome reads keys and typed text in the Frame cycle: the arrows,
  Home and End, Page Up and Down (with Ctrl or Cmd for the document, Alt or Ctrl for
  words, Shift to select), Backspace and Delete (by word with Ctrl or Alt), Ctrl or Cmd
  with A, C, X, V, Z and Shift+Z (or Y). A double press selects a word, a triple a line;
  a drag selects and scrolls past the edge.
- **Composition.** While an input method composes, its text is drawn at the caret,
  underlined, with its own caret, and is not in the buffer or the model until committed.
  Krome tells the platform where the caret is, so the candidate window opens beside it.
- **The caret blinks** while its window has focus: 530 ms on, 530 off, asking for a frame
  at each change (the window paces on demand otherwise). A still screen with no focused
  `edit` still costs nothing.
- **The cursor.** `cursor` is a property: `arrow`, `ibeam`, `hand`, `move`, `resize-ew`,
  `resize-ns`, `forbidden`. An `edit` is `ibeam` unless it says otherwise; the node under
  the pointer that says one sets it.

### The platform

- **Text as events.** `TextTyped { text[32] }` for each commit (a long one in several),
  and `TextComposing { text[64], caret, selection }` while an input method composes (an
  empty one ends it), from SDL3's `TEXT_INPUT` and `TEXT_EDITING`, Win32's `WM_CHAR` and
  `WM_IME_COMPOSITION`, Android's `commitText` and `setComposingText`, and iOS's
  `insertText` and marked text. Events, not the tick's input state, so the Frame cycle
  sees every one once however frames and ticks fall. `InputState::text` stays for games
  that read it.
- **The input method's area.** `platform_text_area(e, rect)`: where the caret is, in the
  window's pixels, so the candidate window and a phone's keyboard keep out of its way.
- **The clipboard.** `platform_clipboard_get(e, out)` and `platform_clipboard_set(e,
  text)`: UTF-8 text only.
- **The cursor's shape.** `platform_cursor(e, shape)`; nothing on a touch screen.
- **Which hosts are built here:** SDL3, which is Linux and macOS, tested on Linux. Win32,
  iOS and Android get stubs: no text events beyond what they deliver today, an empty
  clipboard that takes nothing, no cursor shape, the area ignored. Filled in on those
  machines.

### A game's `Text` and `Paragraph`

`Text::wrap` (units; 0, the default, is one line) wraps a label through `typeset`;
`Text::line_height` with it. `Text::subpixel` asks for subpixel positioning. The 64-byte
string stays the cheap default.

**`Paragraph`** is a second component for a label that needs more: a handle to a string
the text module keeps (`text_paragraph_set(e, string, utf8)`, any length), a wrap width,
and how many characters are shown (`shown`; all by default). A kind with `Transform2`,
`Text` and `Paragraph` draws the paragraph in the `Text`'s font, tint and layer instead of
`Text::str`. The handle is hashed as any field is, and sim code writes the strings, so a
replay writes the same ones.

`typeset` also says where a page ends: the byte offset of the last line that fits a
height (`typeset_fit`), so a game can show a paragraph a page at a time and reveal it a
character at a time with `shown`. Pausing the reveal at a page's end, as arcanum's
dialogue will want, is the game's (or `ui`'s) to do with that; nothing here does it.

## Out of scope

Shaping beyond kerning: ligatures, combining marks drawn as one, complex scripts,
right-to-left, bidirectional text. Hyphenation. Rich text in Krome (`{*tag}` spans; `ui`
has its own). The text input protocol beyond composition (K25: dictation, autocorrect,
the accent menu, surrounding text), and the accessibility tree (K26). Tab order and
shortcuts (slice 4). LCD anti-aliasing. Distance-field glyphs. Spell checking. Hinting
(FreeType's or our own): snapping and a contrast curve instead. A password field (`secret`,
slice 4 with the widgets). Typing-out reveals and paging in a game or `ui` beyond
`shown` and `typeset_fit`.

## Requirements

| ID | Requirement | Priority |
|---|---|---|
| KT1 | Glyphs rasterized on demand per strike (font, pixel size), into R8 pages packed by shelves | P0 |
| KT2 | `renderer_texture_update`: rectangles on Vulkan; Metal and D3D12 upload the page whole, through the render core, until filled in | P0 |
| KT3 | Exact pixel sizes: Krome asks for size times scale; a scale change makes a new strike | P0 |
| KT4 | A fallback list per font; a code point drawn from the first face that has it | P0 |
| KT5 | Crisp small text: whole-pixel baseline, pen and box; rounded vertical metrics; a contrast curve under 14 px | P0 |
| KT6 | Subpixel positioning on request: quarter-pixel copies made as asked | P1 |
| KT7 | `typeset`: breaking, alignment, line height, measure; caret, hit, selection and movement geometry | P0 |
| KT8 | The edit buffer: operations, selection, grouped undo and redo; tested headless | P0 |
| KT9 | Krome's `text` wraps, `nowrap`, line alignment, `line-height` | P0 |
| KT10 | Layout width, then height; slice 1's layout tests unchanged | P0 |
| KT11 | `edit`: one line and many, readonly, placeholder, binding, submit, `change` and `blur` events, clipping and scrolling to the caret | P0 |
| KT12 | Focus by pointer; keys; mouse selection with double and triple presses and drag | P0 |
| KT13 | `TextTyped` and `TextComposing` events, `platform_text_area`, on SDL3 | P0 |
| KT14 | Composition drawn inline at the caret; committed text into the buffer | P0 |
| KT15 | The clipboard: get and set UTF-8 on SDL3 | P0 |
| KT16 | The cursor's shape: the `cursor` property and `platform_cursor` on SDL3 | P1 |
| KT17 | The caret blinks, asking for frames only while an `edit` has focus in a focused window | P0 |
| KT18 | `Text::wrap`, `line_height`, `subpixel` for a game; rocks' pause hint wraps | P0 |
| KT18a | `Paragraph`: strings of any length, a wrap, `shown`; `typeset_fit` for pages | P1 |
| KT19 | The vocabulary (`kimb`): `edit`, its flags, `change`, `blur`, `line-height`, `caret-color`, `selection-color`, `cursor`, `nowrap`, `subpixel` | P0 |
| KT20 | `krome/std`: `input` and `textarea`; `cursor=hand` on buttons | P1 |
| KT21 | Win32, iOS and Android: stubs for the area, the clipboard and the cursor, so they build | P1 |
| KT22 | `ui`'s labels on `typeset` | P2 |
| KT23 | The 9, 11 and 13 px pictures; the numbers; the READMEs; the master PRD corrected | P1 |

## Milestones

Each leaves the tree building and every example running.

1. **Glyphs** (KT1 to KT6). Strikes and pages, fallback, exact sizes, snapping and the
   contrast curve, subpixel copies; `renderer_texture_update`; the font API's callers
   (`text`, `ui`, Krome) moved over. The first pictures at 9, 11 and 13 px, to settle
   the crispness question before anything is built on it.
2. **Paragraphs** (KT7, KT9, KT10, KT18, KT18a). `typeset`; Krome's text wraps and layout
   goes width then height; `Text::wrap`; `Paragraph`; rocks' pause hint.
3. **The platform** (KT13, KT15, KT16, KT21). Text events, the input method's area, the
   clipboard and the cursor on SDL3; stubs on the other hosts.
4. **`edit`** (KT8, KT11, KT12, KT14, KT17, KT19, KT20). The buffer; the node; focus,
   keys, the pointer; composition; the blink; std's `input` and `textarea`; a notes field
   in kromedemo, driven by `--demo` (typed and composed text through a new
   `input_text_override`, as `input_pointer_override` drives the pointer now).
5. **Close** (KT22, KT23). `ui` on `typeset` if it is cheap; the pictures and the numbers
   (a page's upload, a strike's first frame, a 10,000-character paragraph's layout, a
   keystroke in a 1,000-line `edit`); the READMEs; the master PRD corrected where this
   slice proved it wrong.

## Decided

- **No FreeType.** Small text is made crisp by snapping and a contrast curve over
  stb_truetype's coverage; the 9, 11 and 13 px pictures in milestone 1 say whether it is
  enough before anything is built on it.
- **Paragraphs are `typeset`'s**, a module of their own that Krome, `text` and `ui` link.
- **A game's long text is `Paragraph`**, beside `Text`. Paging and pausing a reveal at a
  page's end (arcanum's dialogue) is the game's, from `typeset_fit` and `shown`.
- **Updating part of a texture** is Vulkan's in this slice; Metal and D3D12 re-upload the
  page whole until filled in.
- **Win32, iOS and Android get stubs** for the area, the clipboard and the cursor.
- **`edit` has three events**: submit (`->`), `change` and `blur`.
- **The password field waits** for the widgets (slice 4).

## As built

Nov 15, 2026, on branch tool-stuff: `7a0f053` (1), `888ea2f` (2), `7fa8c56` (3), `8413c16`
(4), and the commit with this section (5). Every done-when holds on Linux but two that are
yours to judge or that the platforms decide (below). Metal, D3D12, Win32, iOS and Android
build untested.

- **Glyphs** (1). A font asset is a strike over faces read once per file; glyphs are made the
  first time they are asked for, from any thread (ASCII lock-free once made, the rest under
  the strike's lock), into 512x512 pages packed by shelves, and a `Glyph` names its page.
  Pages stay **RGBA8**, not R8: the renderer has no one-channel format, and the sprite and UI
  shaders read coverage from alpha. Nothing is rasterized up front; the old fixed set is now
  only where kerning is read (pairs outside it are not kerned). `asset.fonts_flush` uploads
  what changed after the text drawers: the rectangle on Vulkan (`renderer_texture_update`),
  the page on Metal and D3D12, through the render core, until they are filled in.
- **Crisp small text, without hinting** (1). The contrast curve on the coverage this PRD
  planned made 9 px text heavier, not crisper: the swapchain is sRGB, so coverage blends in
  linear light, which thickens light ink on a dark ground and thins dark ink on a light one.
  The correction moved to the UI shader, by the ink's own luminance (light ink thinner, dark
  fuller), and the glyphs keep stb_truetype's coverage. Metrics round to whole pixels and a
  glyph's box sits on whole pixels from the pen. Beside FreeType's hinted DejaVu (Pillow's,
  standing in for the platform's) the weight now matches; the stems are a little softer,
  since FreeType grid-fits them. **Whether that is crisp enough is yours to judge**
  (`kromedemo --type --shots DIR`). The game's sprite path (`text`) does not have the
  correction yet.
- **Fallback** (1) is shown with arcfire's PublicPixel behind DejaVu in `asset_tests` (CJK
  that DejaVu lacks: 元, 円). kromedemo does not ship a fallback: PublicPixel's license is not
  recorded in the repository, and no CJK font is vendored.
- **Paragraphs** (2). `typeset` as planned. Layout answers a node's `narrowest` width too: a
  hugging node gets no more than its room unless it cannot be narrower, and when any text
  wrapped, heights are laid out again at the widths given and the tree placed again. Painting
  decides a wrap by layout's width, not the pixel-rounded rect (4: rounding had wrapped the
  last letter of menu items given exactly their line). `Text` gained `wrap`, `line_height`
  and `subpixel` (no recorded replay holds a `Text`); `Paragraph` keeps strings in the text
  module's table, and the module's own `text.label` carries one (a read of `*.Paragraph`
  needs a kind that has it).
- **The platform** (3). `TextTyped` and `TextComposing` events through one `input_typed`;
  `platform_text_area`, `platform_clipboard_*` (with the process's own clipboard, which is
  what a headless run or a stub host gets back), `platform_cursor`; stubs elsewhere. Key
  overrides and composition overrides drive demos and tests. `Key` gained Delete, Home, End,
  PageUp and PageDown, appended (the input state's size, and so replays, unchanged).
- **`edit`** (4). As planned, with these found on the way: an input method answers every
  telling of the caret's area with its state, so the area is told only when it moves, and is
  where a composition began, not the composition's own caret; a press and a drag in the same
  frame keep the press (a double press is not a drag); an edit's bound text is read at full
  length (a 1 KB buffer had truncated it). The blink wakes an on-demand window at each turn
  through `engine_want_frame_at`, new in core. The focus survives a rebuild by id, and goes
  with its screen or overlay.
- **The gallery's UI picture** (5) changed with the shader's coverage curve: its Vulkan
  reference is made again. **Metal's and D3D12's need making again on those machines.**
- **Not done:** `ui`'s labels on `typeset` (KT22, P2): `ui` keeps its own breaking. The
  9 px comparison against a GTK and a macOS screenshot: against FreeType instead, here.

**Numbers** (Linux, AMD 780M, the dev build):

| what | measured |
|---|---|
| a strike made Ready (DejaVu, its kerning) | 1.1 to 1.4 ms (was 4.9 ms with the fixed set rasterized) |
| glyphs made on demand, 14 px | 2.4 us each (214 in 0.51 ms); asked again, 2 ns |
| a 10,000-character paragraph laid out at 600 px | 1.4 ms, 225 lines |
| a keystroke in a 45 KB, 1,000-line edit | 3.9 ms over the two frames it takes (the whole text laid out again each frame) |
| kromedemo, Lua against markup, with the notes field: form, menu, About, the edit, the composition | largest channel difference 0 (when the desktop's pointer is not over the window) |
| render-gate, Linux | passes, with the gallery's UI reference made again |
| tests | core, kim, kimb, kimc, typeset 43, asset 40, text 31, platform_hid 251, krome 228, goldberg, ui all pass |

**Open:** hinting, if the pictures say so; laying out only the lines a keystroke touches in a
long `edit`; `ui` on `typeset`; the hosts' text input, clipboard and cursor; the text input
protocol beyond composition (K25).

