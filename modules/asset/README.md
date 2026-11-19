# asset

Files that become engine resources: TrueType fonts, WAV or Ogg sounds and streams, cooked
SVG shapes, and blobs (a file's bytes, for a module that parses a format of its own). You
ask for a path and get a handle back at once, the file loads in the background through the
platform's I/O queue, and an event tells you when it is real. Nothing in a game waits for a
file.

The module draws nothing and links no renderer, so a tool can have assets. Textures and
materials, which are the renderer's, are **asset_render**'s (`modules/asset_render`): asset
kinds registered here like any other module's, so scopes, reload and the events treat them
as this module's own. A program that draws links both.

## How it works

Every load has the same shape:

1. **The call returns a handle** to a record that is `Pending`. (asset_render's kinds make
   their renderer objects here, so a texture's slot or a material's row is usable at once.)
2. **A read is submitted** to the platform's `IoQueue` for `<exe dir>/assets/<path>`,
   with the asset handle as the request's user word. The platform pumps the queue once
   per frame (io_uring on Linux; `fopen` elsewhere) and the completion sits on the
   queue's list until someone reaps it.
3. **The kind's pump reaps it.** In phase `PreSim`, on the main thread, after
   `asset.pump_scopes`, `asset.pump_<kind>` scans the completions for its handles and makes
   the record: decodes the sound, parses the font, reads the shape, keeps the blob's bytes.
4. **An event says so.** `AssetReady{asset}` at the next phase boundary when the record
   turns `Ready`; `AssetFailed{asset, error}` when it fails (`korvid/asset/asset.h`).

A failed load keeps its handle: the game logs it and goes on. The
same path loaded twice is the same handle, found through the name table, so modules can
load what they need without coordinating.

Priorities: loads go in at `IoPriority::Normal`. The queue admits reads within a byte
budget per pump (8 MB by default) so a burst of loads cannot stall a frame; what does
not fit waits for the next pump or for idle time (see `docs/design/idle.md`).


**Fonts** (Krome slice 3). A font asset is a strike: a TrueType file at one pixel size. The
file is read once, whatever sizes use it, and parsed by stb_truetype. Glyphs are made the
first time they are asked for (`font_glyph`, from any thread), rasterized into the strike's
pages (512x512, packed by shelves, a page more when one fills) and kept: a `Glyph*` lives
as long as the strike, and names its page. Nothing is made up front unless `FontDesc::ranges`
asks. A code point the face lacks comes from the first face down the fallback list (the
font's own, or the engine's) that has it, at the same pixel size on the same baseline. The
metrics are rounded to whole pixels; a glyph's box sits on whole pixels from the pen; the
coverage is stb_truetype's, unhinted, and how heavy it reads is corrected where it is drawn
(the UI shader, by the ink's luminance). `font_glyph_at` makes quarter-pixel copies for
subpixel positioning. Kerning is read for the Latin set. The pages are drawn through
`AssetFontPages` (`korvid/asset/font.h`), which asset_render sets: with it each page has a
renderer texture and material and `asset.fonts_flush` uploads what changed before the frame
draws; without it (a tool) the pages stay in memory and the handles are null.
## Reload

A file already loaded can be read again (`korvid/asset/reload.h`): `asset_reload(e, path)`, or
`reload PATH` from the CLI. When its bytes changed, the same handle is made of the new ones, in
the kind's pump, where loads land:

- a **shape** gets the new paths (Krome's jobs draw from copies of their own);
- a **blob** gets the new bytes, and the old ones are freed: a pointer to `data` kept past
  `AssetReloaded` is a pointer to freed memory.

Each sends `AssetReloaded{asset}` and counts one more `generation`. Bytes that hash the same as
before (`hash`) change nothing and send nothing. A file that no longer reads, decodes or parses
keeps what the record had: `AssetFailed{asset, error, reload = true}`, and the record stays
Ready. A record whose first read is still out is read once more when it lands; a record that
failed is loaded again from the start (`AssetReady` when it works). Fonts, sounds and streams do
not reload yet. Another module's kind reloads when it registers `AssetKindDesc::reload`, and
does it with the same helpers (`asset_reread`, `asset_reloaded`, ... in `reload.h`):
asset_render's textures and materials do. Who calls `asset_reload` is not the module's business: goldberg's connection
(Krome slice 6), the CLI and tests all do.

## Lifetimes

An asset lives while something holds it. The holders are **scopes**
(`korvid/asset/scope.h`): named sets of references a game creates per level, per menu,
per whatever it loads and unloads together. Every load takes a scope; the loads without
one put the asset in the global scope, which is never released, so code that never
thinks about lifetimes keeps everything for the run, as before.

```cpp
Handle town = asset_scope_create(e, "town");
g.grass = asset_load_material(e, "mygame/grass.material.json", town); // asset_render's
g.theme = asset_load_sound(e, "mygame/town.ogg", town);
// ... on AssetScopeReady{town}: everything it holds is Ready or Failed
asset_scope_release(e, town);   // what nobody else holds goes at the next pump
```

The same path loaded by two scopes is one asset with two references. Assets hold their
own dependencies: a material holds its texture, so a texture never goes while a material
draws with it. A count that reaches zero does not retire at once but at the next pump
(`asset.pump_scopes`, the first thing in PreSim): load the next level's scope **before**
releasing the last one, and what the two share never leaves memory.

Retiring destroys what the record made (asset_render's renderer textures and materials
go past the frames in flight), cancels
a read still in the queue (its completion is reaped and dropped), forgets the path (the
next load reads the file again) and makes the handle stale: every accessor answers null.
A sound's samples are the exception to "at once": the audio module may still be mixing
them on the device thread, so they are handed to it and freed when the mixer says every
voice on them has stopped.

## Names

Every record is named, so a save or a tool can say what it means: a file's record by its
path, a font by `path@px`, a scope by its name. A font's pages are named `<path>@<px>:page<N>`
(asset_render's renderer textures for them, and `font:` that for their materials).

From the CLI: `ls /scopes`, `print /scopes/<n>-<name>` (members, state, references),
`scope create <name>`, `scope load <scope> <path>` and `scope release <name>`. `scope load`
knows its kinds by extension (`.ttf`, `.wav`, `.ogg`) and another module's by the suffix it
registered (`AssetKindDesc::suffix`: asset_render's `.png` and `.material.json`).

## Using it

**Declare the dependency.** In your `module.lua`; add `asset_render` for textures and
materials (`modules/asset_render/README.md` walks through those):

```lua
module "mytool" {
  deps = { "core", "platform", "asset", ... },
}
```

**Load, keep the handles, read the record when it is Ready.** The platform must be up, which
it is in any module's startup since the module list is in dependency order:

```cpp
g->level_text = asset_load_blob(engine, "mygame/town.tmj");
// later, or on its AssetReady:
if (const BlobAsset* b = asset_blob(engine, g->level_text); b && b->state == AssetState::Ready)
    parse(b->data, b->size);
```

**React to arrival, if you care.** A handler or a system can:

```cpp
void on_ready(TickContext& ctx, Span<const AssetReady> ready) {
    Game& g = ctx.state_as<Game>();
    for (const AssetReady& r : ready)
        if (r.asset == g.level_text) ...                     // the one you kept
        else if (r.asset.kind() == g.level_text.kind()) ...  // any blob; each kind has its own
}
```

or in a system, `for (const AssetFailed& f : ctx.events->read<AssetFailed>())`, with
`asset_error_name(f.error)` for the log.

**Look from the CLI** while the program runs (`--cli-stdin` or `--cli-port N`):
`ls /fonts /sounds /scopes`, `print` a record.

## Quick reference

Header `korvid/asset/asset.h` (every kind's header includes it):

| name | kind | what |
|---|---|---|
| `AssetState` | enum | `Pending`, `Ready`, `Failed` |
| `AssetError` | enum | `None`, `NotFound`, `Read`, `Decode`, `Upload`, `Parse` |
| `AssetReady{asset}` | event | the asset turned Ready; `asset.kind()` says which kind |
| `AssetFailed{asset, error, reload}` | event | the asset failed. `reload`: a reload's read could not be used, and it keeps what it had |
| `AssetReloaded{asset}` | event | the asset was made again from its changed file: the same handle, new contents |
| `asset_error_name(err)` | function | `"not found"`, `"decode error"`, ... |
| `KV_ASSET_PATH_MAX` | constant | 128 bytes, NUL included |

Textures and materials (`korvid/asset_render/texture.h`, `material.h`) are asset_render's.

Header `korvid/asset/font.h`:

| name | kind | what |
|---|---|---|
| `FontAsset` | record | `path`, `state`, `error`, `texture`, `material` (the first page's), `px`, `ascent`, `descent`, `line_height` (whole pixels), `strike` (the glyph cache), `kern_pairs`/`kern_values` (sorted); for a table built by hand, `codepoints`/`glyphs`, `ascii`, `replacement` |
| `Glyph` | record | `uv`, `size`, `offset`, `advance`, and its page's `texture` and `material` |
| `FontDesc`, `FontRange` | records | `px`; `ranges` made at load (none by default); `fallback`, faces after this one by asset path |
| `asset_load_font(e, path, px)`, `asset_load_font(e, path, desc[, scope])` | functions | handle at once; px in [4, 256]; a file is read once for every size of it |
| `asset_set_font_fallback(e, paths)` | function | the fallback list for fonts loaded from now on with none of their own |
| `asset_font(e, h)` | function | the record, or null if stale |
| `font_glyph(font, cp)`, `font_glyph_or_replacement(font, cp)`, `font_glyph_at(font, cp, quarter)` | functions | the glyph, made on the first ask (from any thread), from the first face that has it; a quarter-pixel copy for subpixel positioning |
| `font_kern(font, a, b)`, `font_measure(font, utf8, scale)` | functions | kerning (the Latin set), a line's width |
| `font_generation(font)`, `font_counts(font, &glyphs, &pages)` | functions | grows when a fallback lands (lay text out again); what has been made |
| `font_index(font)` | function | fills the ASCII index and U+FFFD from `codepoints`, for a table built by hand |
| `AssetFontPages`, `asset_font_pages(e, pages)` | record, function | how pages are drawn: `create`, `upload`, `destroy`; asset_render sets it |

Header `korvid/asset/sound.h`:

| name | kind | what |
|---|---|---|
| `SoundAsset` | record | `path`, `state`, `error`, `rate`, `channels`, `frames`, `samples` (interleaved f32); `seconds()` |
| `asset_load_sound(e, path[, scope])`, `asset_sound(e, h)` | functions | handle at once; the record or null |
| `sound_decode(bytes, size, alloc, out, &error)`, `sound_decode_wav`, `sound_decode_ogg` | functions | the decoders on their own |

Header `korvid/asset/scope.h`:

| name | kind | what |
|---|---|---|
| `asset_scope_create(e, name)`, `asset_scope_release(e, scope)`, `asset_scope_global(e)` | functions | an `asset.scope` handle; release drops each member's reference; the global scope is never released |
| `asset_scope_add(e, scope, asset)` | function | a reference from the scope to an asset already loaded |
| `asset_scope_progress(e, scope)` -> `AssetScopeProgress` | function, record | `members`, `pending`, `ready`, `failed`, `bytes` |
| `AssetScopeReady{scope, failed}` | event | every member settled |
| `asset_scope_name`, `asset_refs(e, asset)`, `asset_lifetime_stats(e)` | functions | for tools: names, counts, `retired`, `samples_out`, `samples_deferred`, `dying`, `scopes` |
| `asset_samples_retire_hook(e, fn, user)`, `asset_retire_done(e, ticket)` | functions | the audio module's half of retiring a sound |
| `AssetKindDesc`, `asset_kind_register(e, desc)`, `asset_kind_unregister(e, kind)` | record, functions | another module's kind: `refs`, `state`, `retire`; optional `bytes`, `path`, `reload`, `suffix` and `load` |
| `asset_hold(e, scope, asset)`, `asset_ref(e, asset)`, `asset_unref(e, asset)` | functions | for a kind's own loads: what a load does with its scope; references between assets |
| `KV_ASSET_SCOPES`, `KV_ASSET_SCOPE_NAME_MAX` | constants | 256 live scopes, 32-byte names |

Every load function takes a scope as its last argument (`asset_load_blob(e, path, scope)`,
`asset_load_font(e, path, desc, scope)`, `asset_load_sound`, asset_render's `asset_load_texture`); the
forms without one use the global scope. Each record has `refs`.

Header `korvid/asset/blob.h`:

| name | kind | what |
|---|---|---|
| `BlobAsset` | record | `path`, `state`, `error`, `data` (with a NUL after it), `size`, `refs` |
| `asset_load_blob(e, path[, scope])`, `asset_blob(e, h)` | functions | a file's bytes as an asset, for modules that parse a format of their own |

Header `korvid/asset/stream.h`:

| name | kind | what |
|---|---|---|
| `StreamAsset` | record | `path`, `state`, `bytes`/`size` (the compressed file), `rate`, `channels`, `frames`, `loop_start`, `loop_end`, `loop_tagged`, `refs` |
| `asset_load_stream(e, path[, scope])`, `asset_stream(e, h)` | functions | an Ogg Vorbis file kept compressed, for the audio module to decode as it plays |
| `StreamDecoder`, `stream_decoder_open/read/seek/close`, `KV_STREAM_DECODER_MEMORY` | record, functions | decoding into the caller's memory, stereo out |
| `stream_loop_from_comments` | function | LOOPSTART with LOOPLENGTH or LOOPEND, in frames |

Header `korvid/asset/reload.h`:

| name | kind | what |
|---|---|---|
| `asset_reload(e, path)` | function | every record loaded from `path` (a shape by its `.svg` too) reads its file again; how many were asked |
| `asset_reload_all(e)` | function | every shape and blob, and every record of a kind with `AssetKindDesc::reload`, read again and compared |
| `AssetReread`, `asset_reread_of(record)`, `asset_reread`, `asset_reread_after`, `asset_reloaded`, `asset_reload_failed`, `asset_reload_unchanged` | record, functions | for another module's kind: reading its records again the way this module's kinds do |

Each shape and blob record (and asset_render's textures and materials) has `hash` (the bytes
it was made from) and `generation` (how many times it was made again).

Header `korvid/asset/module.h`: `asset_module`, the record; kormake puts it in the list.

What the module registers with the engine:

| name | kind | notes |
|---|---|---|
| `asset.font`, `asset.sound`, `asset.blob`, `asset.shape`, `asset.stream`, `asset.scope` | handle kinds | `kv_handle_describe` prints path, state and size, atlas or length; a scope's name and counts |
| `AssetReady`, `AssetFailed`, `AssetScopeReady`, `AssetReloaded` | event types | no schema: the handle is an asset's, not an entity's, so it is never dropped as stale |
| `asset.fonts`, `asset.sounds`, `asset.blobs`, `asset.streams`, `asset.shapes` | resources | the tables; the pumps write them |
| `asset.pump_scopes` | system, phase PreSim, main thread, first | retires what nobody holds (another module's kinds through their `retire`), then sends `AssetScopeReady`; writes every table |
| `asset.pump_fonts` | system, phase PreSim, main thread, after `asset.pump_scopes` | writes `asset.fonts` |
| `asset.fonts_flush` | system, phase Frame, main thread, after the text drawers | the glyph pages that changed, through `AssetFontPages::upload`; nothing without it |
| `asset.pump_sounds` | system, phase PreSim, main thread, after `asset.pump_scopes` | writes `asset.sounds` |
| `asset.pump_blobs` | system, phase PreSim, main thread, after `asset.pump_scopes` | writes `asset.blobs` |
| `asset.pump_streams` | system, phase PreSim, main thread, after `asset.pump_scopes` | writes `asset.streams`: checks each file opens, reads its length and loop points |
| `/fonts`, `/sounds`, `/scopes` | CLI mounts | the records by path; scopes by `<slot>-<name>` |
| `scope create`, `scope load`, `scope release` | CLI commands | lifetimes by hand |
| `reload [PATH...]` | CLI command | these files read again, every one loaded without a path |

Resources a system of yours might name: none. Reach the records through the functions;
they read the module's tables, which only the pumps write.

## Behaviour worth knowing

- **A released asset's handle goes stale at the next pump.** Keep handles per scope and
  drop them with it.
- **Readiness is a tick late.** `AssetScopeReady` comes from the first pump after the
  last member settled, so a level that waits for it starts one tick after its last file.
- **A scope still alive at shutdown is logged** by name with its member count, as the
  leak report does for memory.
- **Paths are names.** `rocks/rock.png` and `rocks//rock.png` are two
  assets to the name table even though they are one file.
- **Limits.** Paths of 127 characters; 16 kinds of other modules.

## Not in this module

Textures and materials (asset_render), eviction under a memory budget (a scope is released
by the game, never by the module), pack files, noticing by itself that a file changed on
disk (something calls `asset_reload`), reloading fonts, sounds and streams, and meshes. The
I/O queue and its budgets are the platform module's; anything on the GPU is the renderer's,
through asset_render.
