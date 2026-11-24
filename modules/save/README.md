# save

Game state to bytes and back. Three layers, each usable without the next:

1. **The writer** (`writer.h`): objects, arrays and scalars written in order. The calls are
   the data model, not a syntax, so the code that writes a save never knows the format.
   JSON is built; KBOR, core's CBOR, is planned behind the same calls (see below).
2. **Fields** (`save.h`): any struct with a field table (every component, every event) as
   an object of its fields by name, and back. A field the file lacks keeps its value, a
   key the struct lacks is ignored, so saves outlive fields being added and removed.
3. **KTables** (`save.h`): a whole ktable slot by slot, generations and free list included,
   restored exactly. Handles into it mean the same entities after the load, and the next
   spawn hands out the handle it would have, so a run continued from a save hashes the
   same as the run that made it.

The module reads and writes bytes in memory. Where they go is the game's: the platform's
I/O queue and `platform_user_dir` give a per-user directory (`arcanum` keeps its
save there).

## Writing

```cpp
SaveWriter w;
w.init(mem_general());                // JSON, pretty-printed
w.begin_object();
w.key("version"); w.int_value(1);
w.key("level");   w.string_value(level_name(e, level_current(e)));
w.key("ktables");
const char* const ktables[] = {"game.player", "game.npc", "game.rock"};
save_ktables(w, e, ktables);
w.end_object();
if (w.finish()) { /* w.out.data, w.out.len */ }
w.free();
```

`finish()` is the one check: a value with no key, an unbalanced end, a container left
open or nesting past 32 all set `failed` (logged once) and make it false.

Numbers are exact. Integers are written in full to 64 bits and the JSON reader keeps
integer literals exact (`JsonValue::integer`, `json_u64`, `json_i64`). Floats get the
fewest digits that read back to the same bits (`0.15`, not `0.150000006`), always with a
fraction or an exponent (`4.0`, not `4`), so they read back as floats. Not a number and
the infinities are the strings `"nan"`, `"inf"` and `"-inf"`. The text is core's JSON
writer's (`json_write_string`, `json_write_f64`, `json_write_f32` in `korvid/core/json.h`),
so a save and a tool's `json_write` spell the same value the same way.

## Fields

```cpp
save_component(w, my_desc, &row);                     // {"hp": 3, "pos": [4.0, 5.5], "owner": "game.player#0:1"}
load_component(doc, *obj, my_desc, &row, remap);      // the fields obj names, into row
```

A scalar is itself, a vector (`vec2`, `rect2`, `color`, an array) is an array of lanes, a
`char[N]` field is a string. A value of the wrong shape is skipped with a warning naming
the field; integers out of a field's range clamp to it.

## KTables

```json
"game.npc": {
  "free": [[4, 2], [1, 3]],
  "rows": [
    {"slot": 0, "gen": 1, "Transform2": {...}, "Actor": {...}},
    {"slot": 2, "gen": 1, "Transform2": {...}, "Actor": {...}}
  ]
}
```

`free` is the dead slots in the order the ktable will reuse them, each with its
generation. `load_ktable` replaces the ktable's contents (`KTable::restore_slots` under it);
previous-tick copies equal the loaded rows, as for a spawn. Every component is saved,
cosmetic ones too: a ktable is in a save because the game wants it back.

Which ktables to save is the game's call. Typically: what the player changed (their
character, things they moved, what is left of a level's enemies). Not what the level
builds again from its map (tiles), and not presentation (menus, labels).

## Handles

A handle is written by its kind's name, never its id (the id is only the order kinds
registered in, which moves when a module is added), and then:

- **by its name**, when the issuer names it: `"render.material@arcanum/actors/OldMan.png:alpha"`,
  `"level.root@level.root"`, `"arcanum.npc@OldMan"`. On load the issuer finds the
  name, so the handle means the same thing in a run that made its materials in another
  order or built its level root afresh. A name nothing answers to now reads as null, with
  a warning.
- **by slot**, otherwise: `"arcanum.npc#3:1"`, remade with today's kind id. That
  is right for ktables the save restores, whose slots come back exactly.

Every renderer texture and material, asset record and named entity has a name (see the
core and asset READMEs), so a game rarely needs more. For what it does, it passes a
`SaveRemap`; it is called for every non-null handle with the default answer:

```cpp
Handle remap(const SavedHandle& saved, Handle remade, void* user) {
    if (saved.name && !strcmp(saved.name, "boss")) return *(Handle*)user; // the boss is someone else now
    return remade;
}
```

Entity names travel with their rows (`"name": "old_man"`), and `load_ktables` restores every
ktable's slots and names before it reads any row, so a handle saved by name into one ktable
finds an entity of another restored by the same load.

## CBOR, later

The format this becomes is KBOR (`docs/design/kbor.md`): CBOR with a key table, so a
repeated key costs a byte. Core reads and writes it already (`korvid/core/kbor.h`:
`kbor_parse` builds the same tree as `json_parse`, `kbor_write` writes one); what save
lacks is a `SaveWriter` that streams it. The plan below stands as written.

A second `SaveFormat` needs two pieces and nothing else changes:

- **Writing**: each `SaveWriter` call becomes its CBOR item. `begin_object` and
  `begin_array` are indefinite-length map and array (`0xBF`, `0x9F`), ended by the break
  byte (`0xFF`), so nothing has to be counted ahead or patched. Keys and strings are text
  strings, integers majors 0 and 1, `f32_value` a single float (`0xFA`), `f64_value` a
  double (`0xFB`, or a single when it is exact), and NaN and the infinities become real
  floats instead of strings. A handle could become a tagged array; the string form works
  as it is.
- **Reading**: `save_parse` calls `kbor_parse`, which decodes into the same tree the JSON
  reader builds (`JsonDoc`). The loaders only walk that tree, so fields and ktables load
  from either format.

The format is the caller's choice per save, so a game could write CBOR and keep JSON for
debugging.
