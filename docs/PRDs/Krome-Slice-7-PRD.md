# Krome Slice 7 — Pictures

Nov 23, 2026 · @starekrow · building (see As built)

## Goal and exit criteria

Krome was built for tools: boxes, text, icons, a theme. Blackwing wants it to draw
something else as well, a skin converted from another player's (`.wal`, `.wmz`, `.vlt`,
`.xmpskin`), where nearly everything on screen is a piece of a bitmap. Its skin import
note asked fifteen things of the engine (`docs/PRDs/Korvid-Changes-For-Blackwing.md` in
Blackwing's checkout, "Found in the skin import note", KR28 to KR42). This slice takes the
five that are about drawing an image: KR28, KR29, KR31, KR32 and KR34. Each keeps
Blackwing's ID.

[Krome slice 8](Krome-Slice-8-PRD.md) takes placement, motion and masks (KR30, KR33, KR35
to KR37), and [engine slice 13](Engine-Slice-13-PRD.md) the windows (KR39 to KR42). KR38,
a screen from a directory of uncooked files, is in none of them yet: see Out of scope.

Nothing here blocks Blackwing. Skin import is in no phase of its master PRD, and its note
gives no workarounds: a skin that needs a capability waits for it. So this slice has no
date, and is drafted to see its size. It is worth having on its own account: a game's HUD
and a tool's toolbar are cut from sheets too.

It is done when:

- A node draws the 20 x 20 at (40, 0) of a sheet and its `hover` look draws the 20 x 20
  beside it, from one texture, with no texel of a neighbour showing at any window scale.
- A 64-state knob follows a bound value. A 12-frame strip at 10 frames a second plays
  twice and rests on its last frame, and the window paced on demand is asleep after it.
- One frame image with 8-pixel borders draws at 200 x 100 and at 640 x 480 with its
  corners unscaled, and a tiled background repeats from its top left.
- A time display draws `12:34` from a strip of digits, and a title with a character the
  font lacks still lays out.
- An image brightens by 1.2 on hover with no second image, and switching theme recolours
  every image that names the theme's token.
- A screen made of all five is the same pixels on Vulkan, Metal, D3D12 and the CPU
  renderer, held to render-gate.
- The screens of `kromedemo`, `kromescreens` and goldberg's window draw as they did.

## What the code has

Checked Nov 23 on `main` (52f9963).

| ID | Where | What is there now |
|---|---|---|
| KR28 | `render/ui.h`, `krome/src/view.cc` | `UiBox` has a `uv` rect, and Krome never sets it: an image is its whole texture, stretched over the node's rect. The UI shader samples textures with the linear sampler only (`ui.slang`, `gSamplers[1]`) |
| KR29 | `krome` | Nothing. `anim` plays flipbooks for sprites; `sprite_ui` has "images and strips of them" for a game's UI. Neither is Krome's |
| KR31 | `krome`, `sprite_ui/src/layout.cc` | Krome stretches. `sprite_ui` cuts a frame into nine quads (`nine`) for sprites |
| KR32 | `asset` (`font.h`), `typeset` | A font asset is a strike: a TrueType file at one pixel size, its glyphs made into pages on demand. Krome asks for glyphs by strike; `typeset` measures by the strike's metrics |
| KR34 | `krome/tree.h`, `ui.slang` | `KromeLook::tint`, a colour the texel is multiplied by, each channel at most 1. `transition` eases it |
| all | `kimb/src/kimb.cc` | The vocabulary is a fixed table (`k_props`): a new property is a row there, a case in `krome_set_prop`, and a line in the module page |
| all | `renderer-cpu` | Draws the UI list itself, not through `ui.slang`: what the shader learns it learns too |

## A region (KR28)

**`region=x,y,w,h`** on a look, in the file's pixels: the image drawn is that rectangle of
it. It is a look's property, so a state can move it:

```
image "skin/buttons.png" region=40,0,20,20 {
    hover   { region=60,0,20,20 }
    pressed { region=80,0,20,20 }
}
```

A node with a region measures as the region's size, not the file's. A region that
reaches outside the file is an error naming both sizes, when the texture lands. Shapes
(`.svg`) take no region.

**Sampling.** Linear sampling at a region's edge reads the texel beside it, which in a
sheet is another picture. Two changes to the UI primitive:

- `UiBox::uv` is honoured, and the shader holds each sample half a texel inside it.
- `UiBox::nearest`: the nearest sampler. Krome sets it from a new flag, `pixel`, on the
  node, inherited by what is under it, so a skin sets it once at its root. A skin drawn
  at twice its size should be squares, not blur.

One texture, however many nodes cut from it: `image_for` already keeps one handle a path.

## Frames (KR29)

A strip is N frames of one size, stacked down the image or laid across it.

| property | what |
|---|---|
| `frames=N` | the image (or its `region`) is N frames; down, or across with the flag `across` |
| `frame=3`, `frame=@index` | which one, from 0, held inside the strip |
| `frame-at=@level` | which one by a fraction: 0 the first, 1 the last |
| `fps=10` | plays from where it is, at that rate, on the Frame cycle's clock |
| `loops=2` | how many times; 0 for ever. It rests on its last frame |

`frame` and `frame-at` are bound like any property: set when built and when the key
changes, with no rebuild. A playing strip asks for its next frame at the time it is due
(`engine_want_frame_at`, as a caret's blink does), so an on-demand window wakes for each
frame and no more often, and a strip at rest, or one never played, costs nothing. A
hidden window draws no frame; a strip there is where its clock says when it is seen
again. A strip that ends says so once, to the node's event if it has one (`-> done`).

## Fit (KR31)

**`fit=`** says how an image fills a rect that is not its size:

| value | what |
|---|---|
| `stretch` | as now, and the default |
| `natural` | its own size, at the node's alignment, clipped to the rect |
| `tile` | repeated from the rect's top left, the last row and column cut |
| `slice` | stretched between fixed borders: `slice=8`, or four numbers (left, top, right, bottom), in the file's pixels |

`tile` is one box: the shader wraps the sample inside the region, so a 640 x 480
background of an 8-pixel tile is not 4,800 items. `slice` is up to nine boxes, fewer where
a border is 0, the corners at their own size times the window's scale. A rect smaller than
its borders shrinks them in proportion, as `sprite_ui`'s `nine` does. All of it applies to
a region and to the current frame of a strip.

## Bitmap fonts (KR32)

A font that is a picture: `font="skin/digits.kbf"`, a small text file beside its image.

```
image  "digits.png"
cell   9 13
chars  "0123456789:- "
missing " "
```

`chars` is the cells in order, left to right and then down. `missing` is the character
drawn for one the font lacks; left out, such a character draws nothing and takes no room.
An optional `advance` line gives a width other than the cell's.

`asset_load_font` on such a path gives a strike like any other: the size is the cell's
height, the page is the image, each glyph a region of it, no kerning. Everything above
the asset module is unchanged: `typeset` measures it, a text node draws it, `{name}` fills
it. `size=` on the node scales it (26 for a 13-pixel cell is twice), with the nearest
sampler when the node is `pixel`. A text's `color` multiplies it, so a white strip takes
the theme's colours and a coloured one is drawn under `color=white`.

Every cell is the same size. Proportional bitmap fonts (a width a glyph) are not asked for
and not here.

## A look computed from pixels (KR34)

Three properties of a look, applied to the image's texel before `tint`:

| property | what |
|---|---|
| `gain=1.2`, `gain=1.2,1.1,1.0` | each channel times a number, which may be above 1; the result held at 1 |
| `hue=30` | the hue turned by degrees |
| `saturation=0.5` | 0 is grey, 1 is as drawn, above 1 is more |

They are looks, so `hover { gain=1.2 }` is XMPlay's `scaler_lit` with no second image,
and `transition` eases them with the colours. A theme's token can be any of them (`theme
ember { skin-hue 40 }`, `hue=skin-hue`), so `krome_theme` recolours every image that
names it: a `.wal` colour theme is a theme.

The UI item grows by five numbers, in the list's words, the shader and the CPU renderer.
A box with none of them set takes the path it takes now.

Confining a look to a mask (XMPlay brightens only the pixels a control's mask covers) is
KR33's, in slice 8: here a look applies to the node's whole rect.

## Out of scope

- **Masks**, for hit testing and for confining a look: KR33, slice 8.
- **A screen from a directory of uncooked files** (KR38). It is in no slice yet because it
  meets two things the master PRD decided: the runtime reads only `.kimb`, and `krome`
  does not link `kim`. A program can link `kim` itself and hand the result to
  `krome_provide`, which keeps both. What is then missing is not Krome's: images, fonts
  and scripts are read through the asset module, which would have to resolve a skin's
  paths inside its directory and nowhere else. That is a decision about `asset` roots, to
  be made before it is drafted.
- **Proportional bitmap fonts**, and fonts cut by colour key rather than by cell.
- **Timelines**: a strip plays at one rate. Anything more is a script's, with slice 8's
  timers.
- **The converters themselves**, which are Blackwing's.

## Requirements

| ID | Requirement | Priority |
|---|---|---|
| KR28 | `region` on a look; `UiBox::uv` honoured and held inside its edge; `pixel` and the nearest sampler | P1 |
| KR29 | `frames`, `across`, `frame`, `frame-at`, `fps`, `loops`; a playing strip wakes the window only for its next frame | P1 |
| KR31 | `fit=stretch`, `natural`, `tile`, `slice`; `slice=` borders | P1 |
| KR32 | A bitmap font as a font asset, used by a text node as any strike is; `missing` | P1 |
| KR34 | `gain`, `hue`, `saturation` on a look, eased, and as theme tokens | P2 |
| KP1 | Every one of them on Vulkan, Metal, D3D12 and the CPU renderer, held to render-gate | P0 |
| KP2 | The module pages and quick references of `krome`, `kimb`, `render` and `asset` change with their headers; `tools/doccheck.sh` reports nothing new | P0 |

## Milestones

1. **Regions** (KR28). `region`, the shader's inset, `pixel`. A sheet in `kromedemo`'s
   gallery with hover and pressed looks, at window scales 1, 1.5 and 2; render-gate's
   reference for it.
2. **Fit** (KR31). `natural`, `tile`, `slice`, over a region. The frame at two sizes and
   the tiled background, measured in a layout test and drawn in the gallery.
3. **Frames** (KR29). The bound frame, then playing: the knob, and the 12-frame strip with
   the loop's wakes counted while it plays and after.
4. **Bitmap fonts** (KR32). The `.kbf` reader in `asset`, goldberg's pass-through for it,
   the time display, the missing character.
5. **Computed looks** (KR34). The item's new words in the core, the shader and the CPU
   renderer; hover by `gain`; a theme switch.
6. **A skin by hand.** One small skin written in `.kim` from a real sheet, as a converter
   would write it, to find what the five still cannot say before a converter depends on
   them.

**Verification.** `kormake test` on linux-debug, linux-dev and linux-ship; `krome_tests`
and `asset_tests` under `--sanitize address,undefined`; render-gate on each backend's
machine at milestones 1, 2 and 5, which change what the shader does. Metal and D3D12 are
written and not verified unless a machine is to hand: the status says which.

## Decided

In review of the draft (Nov 26):

- **Krome draws pictures as well as `sprite_ui` does.** The two meet a game's developer in
  different places, and that is fine: neither moves onto the other here.
- **The names stay as drafted.** They are looked at again after slice 8, once `.kim` files
  have been written with them.
- **`pixel` inherits.** A flag that inherits is probably unavoidable; it is the first.
- **`tile` wraps in the shader**: one box, and one more branch for a textured pixel. A UI a
  little slower everywhere, rather than one knob (a small tile over a large rect) turning
  into thousands of items.
- **A bitmap font is both.** A `.kbf` of its own, which the asset module reads as a strike
  (and `sprite_text` can load one day, beside a `.ttf`), and a block in `.kim`, so a skin
  can keep its fonts inline.
- **KR34 before masks.** Slice 8 brings them shortly after.

## As built

**Milestone 1, Regions** (Nov 26).

- **The UI primitive.** `UiBox::nearest` (head bit 20) picks the nearest sampler. A box's
  uv is four floats now, not two unorm16 pairs: a cell of a 4,096-texel sheet needs better
  than a sixteenth of a texel, and the pairs clamped a uv past 1 (the gallery's image in a
  box asked for 2 x 1.5 repeats and drew one). The item is 24 words. The shader asks the
  texture its size (`GetDimensions`) and holds the sample half a texel inside the uv rect;
  the CPU renderer does the same from its decode. A whole image's edge is now clamped where
  it wrapped, which changes a few edge pixels of every image Krome draws.
- **Krome.** `region` is a look's (`KROME_LOOK_REGION`), so states move it; layout measures
  it before the file is in; paint turns it into the uv; past the file's edge it is said once
  (`krome_error`, both sizes) and the image draws nothing. `pixel` is `KROME_PIXEL`,
  resolved into the look (`KromeLook::pixel`) from the node or any ancestor.
- **Found on the way.** The PRD's own example could not be written: an image took no string,
  and `.kim` had state blocks only in styles. An image's string is its path now, and a
  state's word with a block among a node's children is that state's look (`"looks"` on the
  node, which the builder already took from Lua). `kimb_check` checks them as a style's.
- **Shown.** The gallery's `ui` scene draws a sheet's middle cell nearest and linear at 6x,
  11x and 2.75x; Vulkan's and the CPU's references are remade, and agree with each other
  within the tolerance. kromedemo's gallery has two buttons cut from `skin.png` at 2x and one
  at 1x, with hover and pressed regions; `--demo` presses one.
- **Tested.** `renderer_cpu_tests` draws a red cell between two blue ones at 1.5x to 2.75x,
  linear and nearest, and finds no blue (without the hold it does). `krome_tests`' pictures
  test: the region's parse and its refusals, its measure, a state's region, the source rect
  and its edge, `pixel` inherited, and the PRD's sheet in `.kim` built and laid out at
  scales 1 and 1.5. Window scales 1.5 and 2 are not run as kromedemo pictures: no window here
  can be asked for a scale, so the tests stand for them.
- **Not verified:** Metal and D3D12 (`GetDimensions` on their bindless arrays is untried);
  their `ui` references are stale.

**Milestone 2, Fit** (Nov 26).

- **`fit` and `slice` are a look's**, like `region`, so a state may change them; `slice=`
  alone slices. What an image draws is one pure function, `krome_image_pieces` (the source
  rect, the file's size, the node's rect, the scale and its alignment in; up to nine pieces,
  a tile size and whether to clip out), which paint draws and the test measures without a
  texture.
- **`tile` in the shader**, as decided: `UiBox::tile` (head bit 21, two more words: a box is
  26) is the size of one repeat in pixels; the shader and the CPU renderer take the
  fraction of the pixel's place over it before the half-texel hold, so a tile's edge never
  reads its neighbour either. The `ui` scene tiles the sheet over a 150 x 40 box.
- **`slice` and `natural` are boxes made on the CPU.** Borders are rounded to whole pixels
  at the scale, so pieces share their edges; a rect smaller than its borders shrinks them in
  proportion, and the middle goes when it has no room. Each piece is sampled held inside
  itself, so a border and the middle never blend across their seam. `natural` places the
  image by the node's own alignment flags and gives it a clip only when it is larger than
  the rect.
- **Stretched and tiled images stay the node's box** (its `bg` multiplying the texel, as
  before: nothing that drew before draws differently); natural and sliced ones draw the box
  under their pieces, each piece the tint.
- **Shown.** kromedemo's gallery has a Skin tab: skin.png's tile over the page, its frame at
  200 x 100 and 400 x 200, a button at its natural size centred in a taller rect; `--demo`
  takes its picture. The 640 x 480 frame is the test's.
- **Tested.** The pieces at 200 x 100, 640 x 480 and at scale 2 (corners 8 and 16 pixels), in
  a rect smaller than the borders (four corners, shrunk), with borders of 0 (three bands);
  the tile at scale 1.5; natural centred, at the end, and clipped. `renderer_cpu_tests`
  draws a tile every 4 pixels and reads the row back from the rect's left edge.
- **Not verified:** Metal and D3D12, as in milestone 1.

**Milestone 3, Frames** (Nov 26).

- **All a look's**: `frames`, `frame` and `frame-at` (one field; whichever was said last),
  `fps`, `loops`; `across` is a node flag, as the PRD has it. So a state can play a strip
  (`hover { fps=12 }`), and every one can be bound. `krome_image_source` takes the frame and
  cuts the strip's cell out of the region; layout measures one frame.
- **Playing is a record per node** (`KromeModule::plays`), kept as a transition's is: begun
  on the Frame cycle's clock at the first paint that plays it, or again when what it plays
  changes (the image, rate, loops, still frame, direction); dropped when its tree is built
  again or a paint no longer finds it playing (a hidden node, `fps` back to 0). The
  arithmetic is `krome_strip_frame`, pure: from the still frame, `loops` times through,
  resting on the last frame.
- **Wakes.** Each Frame cycle repaints the screens whose strip has a frame due, and asks
  for the earliest due frame (`engine_want_frame_at`, the clock's due time turned into
  `time_now`'s). A screen with nothing due asks nothing.
- **The event comes after the paint.** A strip that rests marks its record, and the frame
  fires its node's event once, past the windows' paints, since a handler may rebuild the
  tree being painted. It goes through `e.events`, as paint's KromeView does: giving
  `krome.paint` a context would make every emit from paint need declaring.
- **`krome_settled` waits for a strip that plays a number of times** to rest (one that plays
  for ever is not waited for), so kromedemo's pictures stay the same run to run.
- **Shown.** kromedemo's Skin tab: a 64-frame knob (`knob.png`) on `frame-at=@dial`, beside a
  slider; a 12-frame spinner (`spinner.png`, across) that Spin plays twice at 10 a second.
  `--demo` presses Spin, waits for Krome to settle (a new step, `Settle`), and finds the
  spinner's `spun`.
- **Tested.** The still frame by number and by fraction, held inside; the cell down a region
  and across a file; `krome_strip_frame` at and either side of its due times, from a frame
  other than the first, for ever, still; a strip measured as one frame. Running: the knob
  bound and moved with no rebuild; a still screen paints nothing and asks no frame; the
  strip played at 10 a second, twice, is 24 paints over 2.4 seconds of 60 frames, a frame
  asked on every frame while it plays and on none after, its event once, and nothing more
  in a second at rest.
