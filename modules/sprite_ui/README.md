# sprite_ui

Menus, HUDs, dialogue boxes and whole screens, as trees of named nodes the module owns.
A screen comes from a document (`.spui.json`) or is built in code; the module lays it out
against the window, draws it as screen-space sprites, reads the action map for the topmost
screen that takes input, and tells the game what happened with `SpuiEvent`s by node name.
Game code never positions a label.

## How it works

**Documents** (`spui_load`) are assets of the asset module's scopes (`sprite_ui.doc`): a style
(the font, colors by name, 9-slice frames, images and strips of them, sounds) and named
screens. The document holds its textures, materials, sounds and fonts and turns `Ready`
when they have settled.

**Screens** (`spui_open`, `spui_open_empty` + `spui_add`) are trees of nodes:

| type | what it is |
|---|---|
| `box` | a container; children placed by anchor, pivot and offset |
| `panel` | a box with a 9-slice `frame` behind it, or a flat `fill` color |
| `image` | a style image (a frame of a strip), or a texture rect set by code |
| `label` | rich text: wraps at its width, aligns, reveals |
| `stack` | children in a column or a row, with a `gap` |
| `list` | a column of items with a focus, a cursor (the style image `cursor`), sounds and events |
| `scroll` | one child taller than itself, clipped to its rect; `input` scrolls it with the move action |
| `field` | a line of typed text with a caret |
| `meter` | a value out of a maximum, as a row of icons (`pips`, from a strip) or a `bar` |

**Layout.** A node has an anchor and a pivot (the nine points), an offset, padding, and a
size per axis: a number (units), `fit` (its content) or `fill` (its parent's inside). The
window is the screen in units: its height over the document's design height, a whole
number with `pixel: true` so pixel art and pixel fonts stay crisp. The font is loaded at the
pixel size it is drawn at, so glyphs are never scaled. Layout runs every draw; a tree of a
few hundred nodes costs nothing worth measuring.

**Rich text.** `{*c=gold}`…`{/c}` (a style color or `#rrggbb[aa]`), `{*wave}`…`{/wave}`,
`{*shake}`…`{/shake}`, `{*pause=0.4}` (the reveal waits), `{key}` (a model value), `{{` for a
brace, `\n` for a line. A label with `reveal` (characters per second) types itself out,
plays the style's `tick` sound every other character, and emits `revealed`; a confirm
while it reveals finishes it instead of reaching the screen. The style's `shadow` color
draws every glyph again a unit down and right; `shadow_offset` (units, 1 to 8, times a
label's `font_scale`) carries it further, drawn at every unit out to it, a solid drop.

**Input and events.** `sprite_ui.update` (PreSim, every tick) gives the action map to the topmost
open screen that takes input (layer, then opening order). A list moves its focus with
repeat; confirm emits `SpuiEvent{screen, item, select}` (or the item's `event`), cancel emits
`cancel`. Without a list, confirm and cancel are emitted as themselves, which is how a
dialogue pages. Events reach handlers before that tick's Sim. A screen opened with `hold`
holds the sim (`HoldUi`) while it is open.

**Text entry.** The topmost screen's first visible `field` takes the keyboard's typed
text (the platform's text input, which it switches on while it has a field: layouts, dead
keys and input methods apply) instead of the action map, so typing a `w` or a space moves
and confirms nothing. Backspace erases (with the OS's repeat), Enter or a typed newline
submits (`submit`, the length as the value), Escape cancels, and every edit says `changed`.
A demo types with `input_text_override(e, "Rowan\n")`.

**Motions and covers.** A screen opens with `SpuiOpen::enter` (`Fade` or `Slide` from an edge)
and leaves with `spui_close_with(e, screen, motion)`, taking no input on the way out. A cover
(`spui_cover(e, SpuiCover{Fade or Iris, seconds, tint, focus})`) closes over the whole window,
says `SpuiCovered` when nothing shows, stays while `spui_cover_wait` holds it, and opens again
saying `SpuiUncovered`; it holds the sim meanwhile unless told not to. Switching from one mode
to another (the title to the village, the world to a minigame) happens in the `SpuiCovered`
handler, where nobody sees it. The iris is a disc of clear in a generated texture, ringed by
black quads.

**The model.** `spui_model_set_int(e, "hp", 3)` and the float and string forms; labels say
`{hp}`, meters `"value": "=hp", "max": "=hp_max"`. A HUD is a document and two calls a tick.

**Drawing.** `sprite_ui.draw` (the Frame cycle, once before each frame drawn) lays out every open screen by layer and turns
it into quads: frames as nine slices, images, meters, glyphs. A scroll's children are
cropped to it on the CPU, uv and all, so the renderer needs no clip rects. The quads go into
`sprite_ui.sprite` rows (cosmetic, screen space, anchored top-left, sorted by key within the layer),
reused in order, so a still menu sends the renderer nothing.

## Using it

```cpp
// startup
g.ui = spui_load(e, "mygame/ui/game.spui.json", asset_scope_global(e));
SpuiOpen hud;
hud.layer = 90;
hud.input = false;
g.hud = spui_open(e, g.ui, "hud", hud);

// open a menu that holds the sim, sliding up, focused on an item
SpuiOpen menu;
menu.hold = true;
menu.enter.fx = SpuiFx::Slide;
g.menu = spui_open(e, g.ui, "pause", menu);
spui_focus(e, g.menu, "items", "save");

// into a minigame, behind an iris
SpuiCover c;
c.fx = SpuiFx::Iris;
spui_cover(e, c); // ... and in the SpuiCovered handler: close the world's screens, open the game's

// what it says
void on_ui(TickContext& ctx, Span<const SpuiEvent> evs) {
    for (const SpuiEvent& ev : evs) {
        if (ev.screen != g.menu) continue;
        if (ev.event == name_intern("cancel").hash) { spui_close(*ctx.engine, g.menu); continue; }
        if (!strcmp(name_str(Name{ev.node}), "quit")) engine_request_quit(*ctx.engine);
    }
}

// the HUD's numbers
spui_model_set_int(e, "hp", v.hp);
```

The document:

```json
{ "design": [480, 270], "pixel": true,
  "style": { "font": "mygame/ui/font.ttf", "size": 8, "line": 11, "space": 3,
             "colors": { "text": "#ffffff", "gold": "#ffc840", "focus": "#ffc840" },
             "frames": { "panel": { "texture": "mygame/ui/panel.png", "insets": [4, 4, 4, 4] } },
             "images": { "cursor": { "texture": "mygame/ui/arrow.png", "rect": [4, 1, 8, 14], "scale": 0.5 } },
             "sounds": { "move": "mygame/move.wav", "select": "mygame/ok.wav", "cancel": "mygame/back.wav" } },
  "screens": {
    "pause": { "type": "panel", "name": "box", "frame": "panel", "padding": 10,
      "children": [ { "type": "list", "name": "items", "gap": 3, "items": ["Resume", "Save", "Quit"] } ] } } }
```

A list's `items` may be strings (the name is the text, lower-cased, spaces as `_`) or nodes.
Paths name nodes from the root (`box/items/save`), and a last part that is not a child is
looked for anywhere below, so `spui_set_text(e, s, "note", ...)` finds a deep node by name.

## Quick reference

Header `korvid/sprite_ui/sprite_ui.h`:

| name | what |
|---|---|
| `spui_load(e, path, scope)`, `spui_ready`, `spui_reload` | documents |
| `spui_open(e, doc, screen, SpuiOpen{layer, input, hold})`, `spui_open_empty`, `spui_add`, `spui_close`, `spui_is_open`, `spui_top` | screens |
| `spui_find`, `spui_set_text`, `spui_set_visible`, `spui_set_offset`, `spui_set_color`, `spui_set_image`, `spui_set_texture`, `spui_set_value`, `spui_focus`, `spui_focused`, `spui_scroll`, `spui_revealing`, `spui_reveal_all`, `spui_rect` | nodes, by path |
| `spui_model_set_int`, `spui_model_set_float`, `spui_model_set_str` | the model |
| `SpuiOpen::enter`, `spui_close_with`, `SpuiMotion{fx, seconds, from}` | screens coming and going |
| `spui_cover`, `spui_cover_wait`, `spui_covering`, `SpuiCover`, `SpuiCovered`, `SpuiUncovered` | switching modes |
| `spui_field_text` | a field's text |
| `SpuiEvent{screen, node, event, value}` | event; `node` and `event` are `Name` hashes |
| `spui_stats(e)` | screens, nodes, sprites, scale |

What the module registers: the handle kinds `sprite_ui.doc` (an asset kind) and `sprite_ui.screen`; the
kind `sprite_ui.sprite` (cosmetic); the events `SpuiEvent`, `SpuiCovered`, `SpuiUncovered`; the systems `sprite_ui.update` (PreSim, main
thread, every tick; writes `audio.commands`) and `sprite_ui.draw` (the Frame cycle, main thread;
drawing only: reveals, focus and events stay in `sprite_ui.update`, in the tick); the command `spui reload`; the mount `/spui` (`print /spui/pause` lists
a screen's nodes with their rects).

## Behaviour worth knowing

- **UI state is presentation.** It is not hashed and not saved. What the sim needs leaves
  the UI as events, which come from actions, which the demo scripts drive; a run that opens
  menus hashes the same on any thread count.
- **Call it from the main thread.** Handlers and main-thread systems; the UI's own systems
  are main-thread too.
- **A screen opened on a ready document is built at once**, so the caller can set its
  nodes in the next line. On a document still loading it is built at the first draw after.
- **The space.** Some pixel fonts have a space a pixel or two wide; the style's `space`
  (units) sets the gap between words.
- **Fonts per scale.** A window resize to a new scale loads the font again at the new pixel
  size; the old one stays held by the document until it retires.

## Not in this module

Mouse and touch, drag and drop, text selection and a caret that moves, bidirectional text and
shaping, grids and flexible wrapping layouts, nested scrolls, rich-text images inline,
localization tables, and a visual editor. `field` takes one line of UTF-8, appended and
erased at its end.
