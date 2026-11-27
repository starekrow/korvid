# kim

Korvid Interface Markup: the text form of a Krome screen, and the compiler from it to
`.kimb`, the binary form Krome loads (`docs/PRDs/Krome-Master-PRD.md`, slice 2). Depends on
`core` only, so goldberg, `kimc` and a game that reads text link it without Krome; Krome
does not link it.

## How it works

`kim_parse` reads the text in one pass into a tree of its own (each node, line and value
as it was written), then emits the document from that tree in canonical order through
core's `JsonBuilder`; `kim_compile` writes it as KBOR (`kbor_write`). The parser knows the
grammar, not the meaning: an unknown word, style or token is not its error, since what a
word means depends on the preludes a file uses, which Krome resolves at load.

**Canonical.** Every map is in name order: a node's properties, the file's styles, themes,
definitions and screens, a style's looks, a theme's tokens. A repeated property or token
takes its last value. Flags, styles on a node, strings, uses and scripts stay in the order
written, since their order means something. Empty sections are left out. So comments,
spacing and the order of a line's properties change no byte.

**Lines** are a sidecar, `"lines"`, the last member, one per entry in a fixed walk (in
`kim.h`). `KV_KIM_NO_LINES` leaves it out and changes no other byte.

**Errors** are each kept with the file, line and column (up to 32, the rest counted), and
nothing is compiled when there is one. A bad value or token is reported and the line goes
on, so one pass finds most of a file's mistakes; an unclosed block stops it.

## Using it

```cpp
Array<u8> kimb{};
KimErrors errors;
if (!kim_compile(text, len, "game/form.kim", kimb, errors))
    for (u32 i = 0; i < errors.count; ++i) { char line[512]; errors.format_one(i, line, sizeof line); ... }
```

The grammar and the document's shape are in `korvid/kim/kim.h`. Errors (`KimErrors`) and
the meaning (`kimb_check`) are `kimb`'s, which Krome links too. `kimc` (`tools/kimc`) is
the command line; goldberg compiles `.kim` in process (its built-in `kim` converter);
`kbor decode` shows a `.kimb` as JSON.

## Behaviour worth knowing

- **One node to a line.** `row { text "a" slot }` is a text with a flag `slot`; the slot
  goes on a line of its own.
- **A property's range is text.** `w=120..400` is the string `"120..400"`, as a Lua table
  writes it; a node's bare range (`slider 1..99`) is `"r": [1, 99]`.
- **A percentage and a bound value are text too.** `w=40%` is `"40%"` and `w=@left` is
  `"@left"` (no token starts with `@`); a node's `@mode=fast` is `"b": "mode", "bv": "fast"`.
- **A bare word as a value is a string.** Whether `bg=panel2` is a theme token and
  `w=fill` a keyword is Krome's to say.
- **`id=` is not a property.** It is the node's `"id"`.
- **`font NAME { ... }` is a bitmap font**, at the top of a file as a theme is: its words
  (`image`, `cell`, `chars`, `missing`, `advance`) are kimb's to check, and it is the
  document's `"fonts"` (Krome slice 7).
- **A state's word among a node's children is a look**, when a block follows it on its line:
  `hover { region=20,0,20,20 }` is the node's `"looks": {"hover": {...}}`, as a style's is
  (Krome slice 7). A definition named `hover`, `pressed`, `focused`, `disabled` or
  `selected` cannot be used with a block of children.
