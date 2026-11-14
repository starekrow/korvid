# KBOR

Nov 14, 2026. The engine's binary data format: CBOR holding what JSON can say, with one
convention that removes the repeated key names. Written first; nothing is built. The
schema half is a sketch for much later.

## Why

JSON is the right source format: readable, diffable, and git can do something with it.
It is the wrong thing to load. A long list of objects repeats every key in every object,
and the usual cure is to deflate the file, which the engine does not want to do by
default.

KBOR is the generated form of the same data. It is small without compression, it is
read in one pass with almost no allocation, and it converts to JSON and back.

## The format

A KBOR file is one CBOR item (RFC 8949), restricted to what has a JSON rendering, with
one rule about map keys.

**The key table.** Reading front to back, every text-string map key is appended to a
table: the first is entry 0, the second entry 1, and so on. An unsigned-integer map key
is not a key: it is an index into that table, and stands for the name at that entry.

```
[{"name": "a", "hp": 1}, {0: "b", 1: 2}]
```

The second object pays one byte per key instead of the name. Indices 0 to 23 are one
byte, to 255 two, to 65535 three.

- **Every text key appends**, whether or not the name has been seen. A reader needs no
  hash table to keep the indices right.
- **An index refers backwards only.** An integer key at or past the table's length is an
  error, as is a negative one or a key of any other type.
- **The table lasts one top-level item.** There is no table shared across files or across
  messages on a connection.
- **A single object costs nothing.** With no repeats there are no indices, and the file is
  the CBOR any encoder would have written.

Two things follow. CBOR made from JSON by any stock encoder is already valid KBOR, since
it has no integer keys. And any stock CBOR decoder reads KBOR, leaving the indices as
integer keys for a small pass to resolve.

**What is in and out:**

| CBOR | in KBOR |
|---|---|
| unsigned and negative integers | yes |
| floats (half, single, double) | yes, NaN and the infinities included |
| text strings | yes, definite length |
| byte strings | yes, definite length; base64 in JSON |
| arrays and maps | yes, definite or indefinite length |
| `false`, `true`, `null` | yes |
| `undefined`, other simple values | no |
| tags | no |

Indefinite-length arrays and maps stay because the save writer streams (`begin_object`,
`key`, value, `end_object`) and cannot count ahead; they change nothing about the key
table. A consumer that cannot take them gets a normalized file instead (see
"Normalizing").

The extension is `.kbor`. There is no magic number or marker for now.

## Reading

One pass, front to back. The table is an array of names, indexed by integer keys.

- **Names are stored once.** Korvid's reader keeps one copy of each distinct key however
  many times it appears as text; each table entry only points at it. Someone will convert
  a couple of megabytes of JSON with a foreign encoder that never writes an index, and
  that file must not allocate a string per key to load. The index is not deduplicated,
  the string is.
- **In the document's arena.** The copies, the table and the tree all live in one arena
  that is tossed after the parse. The in-engine reader may also have a mode that interns
  keys as names instead, for data such as kinds whose keys are about to be interned
  anyway.
- **The table itself still grows** by one small entry per text key, so that naive file
  builds a transient table of a few hundred kilobytes. If it ever matters, the rule
  changes to "append only a name not yet in the table", and every reader then needs a
  hash.
- **An integer the reader cannot hold is an error.** Integers run from -2^63 to 2^64-1,
  which CBOR stores without ambiguity. A reader with a signed 64-bit integer behind it
  refuses a value above 2^63-1; it does not wrap it.
- **What front-to-back costs.** Less than it sounds: CBOR containers carry item counts,
  not byte lengths, so a subtree cannot be skipped without being walked anyway. What is
  lost is splicing. A subtree's bytes are not a document: one cannot be lifted out, or a
  pre-encoded one dropped in, without re-encoding.

The JSON reader builds a flat tree (`JsonDoc`, `korvid/core/json.h`) that the save loaders
already walk, and the save module planned a CBOR reader that builds the same tree. A KBOR
reader does that: everything that loads from JSON loads from KBOR unchanged.

## Writing

- **Indices are the writer's choice.** A writer that remembers names writes an index for
  every repeat. One that does not want the bookkeeping writes the text again and is
  still correct; compaction is then a pass somebody runs later, or never.
- **A pure function of its input.** Keys go out in the order they came in, so the same
  source gives the same bytes. KBOR has no canonical ordering of its own and needs none.
- **Floats** as a single when that holds the value exactly, a double otherwise. Readers
  accept half floats too. NaN and the infinities are written as the floats they are.

## Normalizing

A tool that takes JSON or KBOR, and a schema when there is one, and writes KBOR in its
tightest form: an index for every repeated key, definite lengths, and with a schema the
typed form of every field. It is a goldberg converter like any other, so it goes
wherever a pipeline needs it: after a streaming writer, in front of a consumer that
cannot take indefinite lengths, or on the way into a pack.

## JSON and back

Conversion with no schema is always defined. A schema, later, makes it lossless.

| | JSON to KBOR | KBOR to JSON |
|---|---|---|
| object, array, string, bool, null | the same | the same |
| number without fraction or exponent | an integer | |
| any other number | a float | |
| integer | | digits only |
| float | | always with a `.0` or an exponent |
| NaN, infinity | (cannot arise: `"nan"` stays a string) | `"nan"`, `"inf"`, `"-inf"` |
| byte string | (cannot arise: a base64 string stays a string) | a base64 string |

- **Integers are exact** from -2^63 to 2^64-1. The JSON reader already keeps an integer
  literal's exact bits and whether it was written as one.
- **NaN and the infinities are strings in JSON**, at the same key, spelled as the save
  module already spells them. Coming back, a writer that knows the field is a float
  encodes the float. One that does not writes the string, and whatever reads it expecting
  a number either copes or breaks. The save loaders already read these strings where they
  expect a float.
- **Lossless within reason.** Korvid's own tools round-trip numbers exactly. A foreign
  tool in the middle that drops the `.0` or cannot hold an integer above 2^53 is the
  toolchain's affair.
- **Byte strings are one-way without a schema**, in the same way. KBOR to JSON and back
  turns a blob into a string of base64. Whether that matters is up to whoever owns the
  file.
- **Base64 is RFC 4648's.** Writing JSON, a byte string always comes out in the standard
  alphabet with padding, unless a schema says otherwise. Reading a field declared as
  bytes, every tool accepts the standard alphabet or the URL-safe one, padded or not: the
  two differ only in characters neither shares, so which one a string is in is never in
  doubt. A string that mixes them, or is not base64 at all, is an error.

## Where it is used

Probably everywhere the engine moves structured data: built assets, the wire between
goldberg and its monitors and targets, and saves.

- **JSON is the source, KBOR is generated.** Nobody checks KBOR in. Diffs, review and
  hashing of sources all happen on the JSON; ordering keys the same way every time is the
  job of whoever writes it.
- **Saves.** The save module wrote its calls as a data model so that a second format
  could sit behind them (`modules/save/README.md`, "CBOR, later"). KBOR is that format.

## Schemas (a sketch, for later)

The system runs without them: implicit KBOR to JSON and back is the whole first version,
and nothing in a schema-less file changes when schemas arrive.

A schema is a JSON file that says what the objects in a file are. The nearest prior art
is JSON Type Definition (RFC 8927).

```json
{
  "kbor-schema": 1,
  "root": "level",

  "enums": {
    "blend": { "alpha": 0, "add": 1, "multiply": 2 }
  },

  "kinds": {
    "level": {
      "fields": {
        "name":    "string",
        "size":    { "type": ["u32"], "length": 2 },
        "objects": ["object"],
        "motion":  "bytes?"
      }
    },
    "object": {
      "union": "type",
      "of": { "sprite": "sprite_object", "trigger": "trigger_object" }
    },
    "sprite_object": {
      "unknown": "error",
      "fields": {
        "pos":   { "type": ["f32"], "length": 2, "finite": true },
        "blend": { "type": "blend", "default": "alpha" },
        "layer": { "type": "i32", "min": 0, "max": 15,
                   "doc": "Draw layer, back to front" }
      }
    }
  }
}
```

**What it holds:**

- **Kinds.** Named object shapes: fields and their types. A type is a string (`"u32"`,
  `"string"`, `"bytes"`, a kind or an enum), `["T"]` for an array of it, a trailing `?`
  for optional. A field with rules is an object.
- **Enums.** Names to integers, numbered explicitly and never renumbered.
- **A root.** The kind of the top of the file.
- **Selectors.** Mostly none: once the root is a `level` and `objects` is an array of
  `object`, each object's kind follows from where it sits, and that walk is also the
  check of the file's structure. A `union` picks the kind by a field's value where
  position is not enough. A path selector (`"layers[].props": "prop_bag"`) for files
  whose shape is someone else's waits until something needs it.
- **Validation.** Per field: required or optional, a range, a length, finite. Per kind:
  what an unknown field is (an error, a warning, or kept).

**What a schema does to conversion:**

| field | JSON | KBOR |
|---|---|---|
| `bytes` | a base64 string | a byte string |
| an enum | `"sprite"` | `1` |
| a float | `"nan"`, `"inf"`, `"-inf"` | the float |
| a number | whichever spelling | the field's type |

A reader with the schema accepts either form of each, since a file converted without it
carries `"sprite"` where the schema would have written `1`, and the other way round.
Without the schema, KBOR to JSON gives `"type": 1` and JSON to KBOR gives
`"type": "sprite"`; whether that is a problem is up to the toolchain and the engine.

**Worth a slot:**

- **Defaults.** A writer omits a field equal to its default and a reader fills it in.
- **Renames.** `"was": ["old_name"]` on a field loads old files after a rename.
- **`doc` and UI hints.** A label, units and a range are enough to generate a form; Krome
  and goldberg's forms could both read them.

**Kept out:** regex patterns and cross-field rules, which belong in code; and seeding the
key table from the schema, which would save each name's first appearance and make the
file unreadable without its schema.

The engine already has part of this. Every component and event has a field table
(`FieldDesc`: a name, a scalar type, a count), which the save module reads and writes by
name. It has no bytes type and no enums. Whether a schema is generated from field tables,
extends them, or stands apart is the first question when this gets built.

## Prior art

- **Smile** (Jackson's binary JSON) shares key names by an implicit table in the order met,
  and optionally short string values.
- **CBOR stringref** (tags 25 and 256) does it for all strings, at two more bytes a
  reference for the tag.
- **Packed CBOR** (an IETF draft) and **Amazon Ion** put an explicit table up front.

## Parked

- **A marker.** The extension is enough until it is not.

## Open questions

- [ ] Schemas against the engine's field tables. Later.
- [ ] Is KBOR a core facility next to `json.h`, or a module?
