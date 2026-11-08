# asset

Files that become engine resources: PNG textures, JSON materials and TrueType fonts. You ask for a path
and get a handle back at once; what the handle names already works (a texture slot that
shows the placeholder checkerboard, a material row with the default look), the file
loads in the background through the platform's I/O queue, and an event tells you when
it is real. Nothing in a game waits for a file.

## How it works

Every load has the same shape:

1. **The call returns a handle.** `asset_load_texture` creates a renderer texture slot
   (`renderer_texture_create`), `asset_load_material` creates a renderer material row.
   Both are usable immediately: a `Sprite` can carry the material, a `MaterialDesc` can
   name the texture. The record is `Pending`.
2. **A read is submitted** to the platform's `IoQueue` for `<exe dir>/assets/<path>`,
   with the asset handle as the request's user word. The platform pumps the queue once
   per frame (io_uring on Linux; `fopen` elsewhere) and the completion sits on the
   queue's list until someone reaps it.
3. **The pump reaps it.** In phase `PreSim`, on the main thread, `asset.pump` scans the
   completions for texture handles, decodes the PNG into the module's allocator and hands
   the pixels to `renderer_texture_upload`; the upload rides the next frame's command
   buffer. `asset.pump_materials` runs after it, parses material files, loads the texture
   each names (step 1 again, deduplicated by path) and writes the renderer row.
4. **An event says so.** `AssetReady{asset}` at the next phase boundary when the record
   turns `Ready`; `AssetFailed{asset, error}` when it fails. A material is `Ready` only
   when its texture is resident, so a sprite never sees a tinted checkerboard turn into
   the real image two frames apart.

A failed load keeps its handle and its placeholder: the game logs it and goes on. The
same path loaded twice is the same handle, found through the name table, so modules can
load what they need without coordinating.

Priorities: loads go in at `IoPriority::Normal`. The queue admits reads within a byte
budget per pump (8 MB by default) so a burst of loads cannot stall a frame; what does
not fit waits for the next pump or for idle time (see `docs/design/idle.md`).

## Using it

**Declare the dependency.** In your `module.lua`:

```lua
module "mygame" {
  deps = { "core", "platform", "math", "render", "asset", "sprite", ... },
}
```

**Load at startup, keep the handles.** The renderer and the platform must be up, which
they are in any module's startup since the module list is in dependency order:

```cpp
struct Game {
    Handle rock_look;     // asset.material
    Handle rock_material; // render.material: the row the file fills
};

bool mygame_startup(Engine& engine, ModuleInstance& self) {
    Game* g = arena_push<Game>(self.arena);
    self.set_state(g);
    g->rock_look = asset_load_material(engine, "mygame/rock.material.json");
    const MaterialAsset* m = asset_material(engine, g->rock_look);
    if (!m) return false;
    g->rock_material = m->material; // usable now; the look arrives
    return true;
}
```

A sprite then carries `g.rock_material` in its `Sprite::material`. For a texture alone,
`asset_load_texture` and `asset_texture(engine, h)->texture` give the renderer's texture
handle to put in a `MaterialDesc` of your own.

**Write the material file.** `assets/rock.material.json` in your module's directory;
kormake copies it to `bin/assets/<module>/`:

```json
{ "texture": "mygame/rock.png", "blend": "alpha", "tint": [1, 1, 1, 1] }
```

`blend` is `opaque`, `alpha` or `additive`; `tint` is `[r, g, b]` or `[r, g, b, a]`,
linear; `params` is up to eight numbers (the sprite shader uses the first two as a UV
offset). Leave out what you do not need.

**React to arrival, if you care.** Most games do not; the placeholder is fine for a few
frames. A handler or a system can:

```cpp
void on_ready(TickContext& ctx, Span<const AssetReady> ready) {
    Game& g = ctx.state_as<Game>();
    for (const AssetReady& r : ready)
        if (r.asset == g.rock_look) ...                      // the one you kept
        else if (r.asset.kind() == g.rock_look.kind()) ...   // any material; a texture's kind differs
}
```

or in a system, `for (const AssetFailed& f : ctx.events->read<AssetFailed>())`, with
`asset_error_name(f.error)` for the log.

**Poke it from the CLI** while the game runs (`--cli-stdin` or `--cli-port N`):

```
ls materials
dump material mygame/rock.material.json
patch material mygame/rock.material.json tint 1 0.5 0.5
patch material mygame/rock.material.json blend additive
patch material mygame/rock.material.json texture mygame/rock2.png
```

A patch changes the renderer row before the next frame and nothing on disk.

**Fonts.** `asset_load_font(engine, "text/DejaVuSans.ttf", 28)` follows the same shape:
the handle comes back at once with a renderer texture (the glyph atlas) and a material
for it, the file reads in the background, and `asset.pump_fonts` rasterizes printable
ASCII and Latin-1 at that pixel size with stb_truetype (vendored under `src/vendor/`),
uploads the atlas as white with coverage in alpha, and fills the glyph and kerning
tables. `font_glyph`, `font_kern` and `font_measure` read them; the text module turns
them into sprites. The same path at the same size is the same handle.

## Quick reference

Header `korvid/asset/texture.h`:

| name | kind | what |
|---|---|---|
| `AssetState` | enum | `Pending`, `Ready`, `Failed` |
| `AssetError` | enum | `None`, `NotFound`, `Read`, `Decode`, `Upload`, `Parse` |
| `TextureAsset` | record | `path`, `state`, `error`, `texture` (render.texture), `io`, `width`, `height` |
| `AssetReady{asset}` | event | the asset turned Ready; `asset.kind()` says texture or material |
| `AssetFailed{asset, error}` | event | the asset failed; it keeps its placeholder |
| `asset_load_texture(e, path)` | function | handle at once; null if the stack is not up or the path is too long |
| `asset_texture(e, h)` | function | the record, or null if stale |
| `asset_error_name(err)` | function | `"not found"`, `"decode error"`, ... |

Header `korvid/asset/material.h`:

| name | kind | what |
|---|---|---|
| `MaterialAsset` | record | `path`, `state`, `error`, `material` (render.material), `texture_asset`, `io`, `desc`, `texture_path` |
| `asset_load_material(e, path)` | function | handle at once, row alive |
| `asset_material(e, h)` | function | the record, or null if stale |
| `asset_material_find(e, path)` | function | the handle a path was loaded as, or null |
| `asset_material_count(e)`, `asset_material_at(e, slot)`, `asset_material_handle_at(e, slot)` | functions | records by slot, for tools |
| `asset_material_patch(e, h, desc)` | function | record and row together; the texture follows the path |
| `asset_material_set_texture(e, h, path)` | function | loads the path and points the row at it |
| `material_parse(text, len, desc, texture_path, cap, &error, &at)` | function | the file format |
| `material_format(desc, texture_path, out, cap)` | function | the record as a file's text |

Header `korvid/asset/font.h`:

| name | kind | what |
|---|---|---|
| `Glyph` | record | `uv` (atlas), `size` (pixels), `offset` (quad center from the pen, +y up), `advance` |
| `FontAsset` | record | `path`, `state`, `error`, `texture`, `material` (alpha, white), `px`, `ascent`, `descent`, `line_height`, `atlas_width/height`, `glyphs`, `kern` |
| `asset_load_font(e, path, px)` | function | handle at once; px in [4, 256] |
| `asset_font(e, h)` | function | the record, or null if stale |
| `font_glyph(font, cp)`, `font_kern(font, a, b)`, `font_measure(font, str, scale)` | functions | lookups over a Ready font; nothing before |
| `KV_FONT_FIRST`, `KV_FONT_COUNT` | constants | 32, 224: printable ASCII and Latin-1 |

Header `korvid/asset/png.h`:

| name | kind | what |
|---|---|---|
| `PngImage` | record | `width`, `height`, `rgba` (caller frees) |
| `png_decode(data, size, alloc, out, &error)` | function | tightly packed RGBA8 or false with a reason |

Header `korvid/asset/module.h`: `asset_module`, the record; kormake puts it in the list.

What the module registers with the engine:

| name | kind | notes |
|---|---|---|
| `asset.texture`, `asset.material`, `asset.font` | handle kinds | `kv_handle_describe` prints path, state and size, blend or atlas |
| `AssetReady`, `AssetFailed` | event types | no schema: the handle is an asset's, not an entity's, so it is never dropped as stale |
| `asset.textures`, `asset.materials`, `asset.fonts` | resources | the tables; the pumps write them |
| `asset.pump` | system, phase PreSim, main thread | writes `asset.textures` |
| `asset.pump_materials` | system, phase PreSim, main thread, after `asset.pump` | reads `asset.textures`, writes `asset.materials` |
| `asset.pump_fonts` | system, phase PreSim, main thread, after `asset.pump_materials` | writes `asset.fonts` |
| `ls materials`, `dump material`, `patch material` | CLI commands | the material tools |

Resources a system of yours might name: none. Reach the records through the functions;
they read the module's tables, which only the pumps write.

## Behaviour worth knowing

- **Handles are forever.** Records are never destroyed before shutdown; a path loaded
  once stays loaded. The renderer's texture and material go with the renderer. Unloading
  is not here yet; a game with levels will want it (see below).
- **The placeholder is visible.** A texture that is not resident draws as the red and
  white checkerboard, tinted by the material. That is on purpose: a missing file shows
  itself. A game that wants a black frame instead waits for its `AssetReady` events.
- **Order of arrival is the disk's.** Texture reads complete in whatever order the queue
  and the kernel finish them, within a priority. A material is reported after its texture
  by construction.
- **Decode is on the main thread.** A 4096x4096 PNG costs milliseconds in `asset.pump`.
  The pump decodes everything that arrived this frame; a burst of large textures shows up
  as a long tick. Cooked formats (BC, ASTC) are the fix and are a later slice.
- **Paths are names.** `example_rocks/rock.png` and `example_rocks//rock.png` are two
  assets to the name table even though they are one file.
- **Material edits do not touch the file.** `patch` changes the row; what you keep is
  what `dump` prints.
- **Limits.** 4096 texture records and 4096 material records per run; paths of 127
  characters.

## Not in this module

Unloading and reference counting, cooked texture formats and pack files, mipmaps, texture
atlases, hot reload of files that changed on disk, formats other than PNG and the
material JSON, and any asset that is not a texture or a material (meshes, sounds,
levels). The I/O queue and its budgets are the platform module's; the pixels' home on
the GPU is the renderer's.
