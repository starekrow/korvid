# Krome

Nov 14, 2026. A UI library for tools and dialogs: screens written in a small markup, laid
out with rows, columns and constraints, styled by name, wired up in Lua. Written first;
nothing is built.

Krome is the name inside: the module, the code, this document. To anyone outside it is
Korvid UI, powered by its Krome engine.

## Why

Tools built on Korvid need screens that look deliberate: inspectors, forms, diff reviews,
status panels, reports. The usual answers do not fit:

- **Electron** gets the look by shipping a browser. Too heavy for a tool that sits next to
  a running game, and not something to embed in one.
- **Qt, Slint, Avalonia, Noesis** each bring a licence or a runtime that a game cannot
  carry and sell without drama.
- **Immediate-mode toolkits** are quick to build with and look like debug overlays.

What makes web UI look good is not HTML or CSS. It is a flex layout model, a handful of
drawing primitives (rounded rectangles, borders, soft shadows, gradients, clipping), good
text, reactive state and hot reload. Krome is those five things with a syntax made for
them and nothing else.

## Where it sits

- **A module, `krome`.** General engine UI. A tool or a game takes it like any other
  module; it draws through the renderer like anything else.
- **Not `ui`.** The `ui` module is game screens: menus, HUDs and dialogue as screen-space
  sprites, pixel-crisp, with 9-slices, typewriter reveals and focus lists driven by the
  action map. Krome is the pointer-and-keyboard kind: dense, resizable, text-heavy. The
  two stay separate for now; whether they ever share a layout pass or a document format
  is an open question.
- **Not goldberg.** goldberg is Krome's first customer and nothing more
  (`docs/design/asset-toolchain-design.md`, "Dialog UI library"). Krome knows nothing
  about asset graphs; goldberg's window is one Krome client among any number.

## Three layers

Krome is small in code and grows in text, the way TeX is primitives, then a format file,
then the document.

| layer | written in | holds |
|---|---|---|
| kernel | C++ | a few node types, layout, paint, input, the parser |
| prelude | Krome markup, with Lua for behaviour | the properties, the widgets, the styles |
| content | Krome markup | screens |

The parser is dumb: every line is a node with arguments, properties and children, and it
accepts anything shaped like that. Meaning comes afterwards, from what the prelude has
declared. Content cannot tell a prelude word from a kernel word.

**The kernel's nodes:**

- `box`: a container. Lays out its children, paints a background, border and shadow, and
  can clip and scroll.
- `text`: a run of text, wrapped to its width.
- `image`: a texture or a vector shape (see "Vector shapes").
- `hit`: a region that takes the pointer and the focus and emits raw events.
- `edit`: an editable line or block of text. In the kernel because caret, selection and
  IME cannot be built from the other four.

Everything else (button, slider, checkbox, tab bar, tree view) is a prelude definition
over these. A game that wants its own widget set and skin swaps the prelude; the kernel
is the same under both.

## The syntax

```
col pad=12 gap=8 {
    row gap=8 {
        label "Name"
        input @name fill
    }
    row gap=8 {
        label "Level"
        slider @level 1..99 fill
        text "{level}" w=40
    }
    spacer
    row [lightbuttons] gap=8 right {
        button "Cancel" -> cancel
        button "Save" primary -> save
    }
}
```

One node per line: a word, then any of these in any order, then an optional block of
children in braces.

| written | is |
|---|---|
| `fill`, `right`, `primary` | a flag: a bare word |
| `gap=8`, `w=120..400` | a property; a range is a minimum and a maximum |
| `"Name"` | the content: a string, with `{name}` for a state value |
| `1..99` | a bare range, for nodes that take one |
| `@name` | a binding to state, two-way where the node edits |
| `-> save` | an event to emit, by name |
| `[darktabs]` | one or more styles, by name |

Braces, not indentation: a block survives being pasted, and an editor can match it.

## Layout

The vocabulary is the one elm-ui, SwiftUI stacks, Clay and Figma's auto layout all arrived
at, and that Figma has shown people who do not program can use.

- **Containers.** `row` and `col` place children one after another; `stack` places them
  on top of each other, each anchored within the parent.
- **Spacing.** `gap` between children, `pad` inside the edge.
- **Size, per axis.** Hug the content (the default), fill the space left over, or a fixed
  number. `w=` and `h=` take a number, `hug`, `fill`, or a range that bounds either. A
  bare `fill` fills along the parent's direction.
- **Spacer.** `spacer` is an empty node that fills. It is how things get pushed apart.
- **Alignment.** Flags on the container, per axis: `left`, `center`, `right`, `top`,
  `middle`, `bottom`.

A top-level screen shell (toolbar, tree, viewport, inspector) may want a grid written as
areas, the way CSS `grid-template-areas` does it. Not designed yet.

## Styles

By habit a node line says where things go and the bracket says what they look like, but
that is the author's choice, not a rule. Any property may be written in either place.
`pad` and `gap` are the plain case: spacing in one screen and density in another,
depending on what the author meant. Krome has no opinion. When both set the same
property, the node line wins.

```
style darktabs {
    bg=panel2 radius=4 text=dim
    hover    { bg=panel3 }
    selected { text=bright underline=accent }
}
```

- **Named, never selected.** A style applies where it is written and nowhere else. There
  are no selectors and no cascade, so "why does this look like that" is answered by the
  line itself.
- **Merged left to right.** `[darktabs compact]` is darktabs, then compact over it.
- **States live in the style.** Hover, pressed, focused, disabled and selected are blocks
  inside the definition. Markup never mentions them.
- **Values are tokens.** `panel2` and `accent` resolve against a theme table. Light and
  dark, or a game's own skin, is one table swapped.
- **A few properties inherit.** Font, text color and accent flow down to children, so a
  style on a row themes the labels inside it. Which properties do is declared, not built
  in (next section).

## The prelude

The prelude declares the properties themselves:

```
prop text-color  color   inherit
prop bg          color
prop radius      number
```

A name, a type, and whether it flows to children. The kernel knows how to paint a
background; that `bg` is the word for it, and that it does not inherit, is text.

And it defines widgets as compositions:

```
def button label {
    box [button] focusable -> press {
        text "{label}"
    }
}
```

A definition is a named subtree with parameters. One slot for children covers "labelled
field" and "panel with a title bar", which is most of the repetition in a tool screen.
How far templating goes beyond that is undecided.

## Control flow

Two constructs, and no expressions beyond looking up a name:

```
each item in assets { row { text "{item.name}" } }
if dirty { badge "unsaved" }
```

Every markup that grew loops, conditionals and expressions turned into a bad programming
language. The rule that prevents it: when a screen needs logic, that part is a Lua
function that returns a tree.

## State, events and Lua

- **State is a table.** A screen is opened against a model. `@name` reads and writes a
  field of it; `{name}` reads one into text. A change to the model redraws what read it.
- **Events are names.** `-> save` emits `save` with the node and its value. Lua handles
  it. The kernel supplies only the raw ones (press, release, drag, key, focus, text).
- **Behaviour is Lua.** A slider's drag logic is code, so a prelude is a pair: definitions
  in markup and handlers in Lua.
- **Markup is sugar.** It compiles to the same tree a Lua table constructor builds, so a
  screen can be written either way, and the markup can arrive after the layout and paint
  are proven from Lua.

## Drawing

One shader's worth of primitives: a rounded rectangle by signed distance, with border,
soft shadow, gradient fill and a clip rect. Images and glyphs are textured quads as they
are today.

**Text is the hard part** and the largest piece of work. Today's `text` module is one
line of up to 64 bytes, drawn as glyph sprites from a `truetype` (stb_truetype) atlas.
Krome needs paragraphs, wrapping, shaping, font fallback, and small sizes that stay
crisp. Homemade UI looks homemade mostly here.

## Vector shapes

Icons and UI art as paths, rasterized by the engine at the pixel size they are drawn at.

- **A shape asset.** A compact list of paths, fills and strokes. SVG is an import format:
  a Lua asset tool converts it, and the runtime never reads XML.
- **Rasterized on demand.** On the CPU, into a texture, at the exact pixel size, cached by
  shape and size. A change of DPI or UI scale renders again. This is the point of having
  it in the engine: the target decides the resolution, not the build.
- **The subset.** Cubic curves and arcs flattened to lines; both fill rules; strokes with
  joins, caps and dashes, expanded to fills; solid, linear and radial paint; transforms;
  clip paths; group opacity. No text, filters, CSS or animation.

## Assets and reload

Screens, preludes, styles, themes and shapes are assets, held by handle. Krome redraws
when a handle rebinds, and does not care who rebound it.

With goldberg in the picture that is the whole of hot reload: a screen file is an asset,
the markup converter reports the prelude, styles and shapes it references as discovered
edges, and saving any of them pushes exactly the screens that use it. Krome itself needs
none of that to run; a pack with the built assets is enough.

## Errors

A dumb parser means every mistake is found late, at the layer that gives words meaning.
So every node carries its file and line through definition expansion and style
resolution, and an error names the line the user wrote, not the kernel node it became.

## Figma import (a side quest)

Figma's auto layout is this layout model, so a design can be pulled in as a starting
point: design the screen there, convert it once, work on the markup by hand from then on.
One way only. From memory of Figma's API; check its documentation before building.

| Figma | Krome |
|---|---|
| auto-layout frame, horizontal or vertical | `row`, `col` |
| item spacing, padding | `gap`, `pad` |
| hug, fill, fixed; min and max | the same three; a range |
| fills, strokes, corner radius, drop shadow | style properties |
| component, instance with overrides | `def`, a use with arguments |
| variants (a state property) | state blocks in a style |
| shared styles and variables | theme tokens |
| vectors and icons | shapes, by way of SVG export |

- **Source.** A JSON dump of the document tree, from a plugin run inside Figma or from
  the REST API. Not the `.fig` file, which is undocumented.
- **What will not come across.** Frames laid out by hand import as a `stack` with anchors
  and do not reflow. Text will wrap slightly differently. Background blur, blend modes
  and masks are dropped with a warning. Figma has no bindings or events; those are added
  by hand afterwards.
- **How it escalates.** A re-import that keeps hand edits, and a plugin as a live monitor,
  are both possible and both a different size of project.

## Not in the design

- A visual editor.
- Animation beyond transitions between style states.
- Accessibility, right-to-left text and IME beyond what `edit` needs. Each needs its own
  design before a tool ships to anyone but us.
- Replacing `ui`.

## Open questions

- [ ] File extension and comment syntax.
- [ ] Templating: parameters and one slot is the sketch. Is it enough?
- [ ] Text: HarfBuzz, or a small shaper of our own for the scripts we need first?
- [ ] The shape filler. stb_truetype exposes its rasterizer for arbitrary outlines, which
      may do for a first version (nonzero winding only); otherwise our own.
- [ ] Grid areas for screen shells.
- [ ] String interpolation: `{name}` here, `{=key}` in `ui`'s rich text. One spelling?
- [ ] Does `ui` ever move onto Krome's layout or documents?
- [ ] Where Krome's update and draw sit in the schedule, and whether a tool window with
      no simulation runs the same phases.
- [ ] Multiple windows.
