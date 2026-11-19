# asset_render

The asset kinds that are the renderer's: PNG **textures** and JSON **materials**, and the
renderer textures of the asset module's glyph pages. The asset module reads files and keeps
records without drawing anything, so a tool links it with no renderer. This module is the
half that draws, for programs that do. A texture or a material is an asset like any other
(the same handles, states, events, scopes and reload), because its kinds are registered with
the asset module (`AssetKindDesc`, `korvid/asset/scope.h`); see `modules/asset/README.md`
for what every kind shares.

You ask for a path and get a handle back at once. What the handle names already works: a
texture slot that shows the placeholder checkerboard, a material row with the default look.
The file loads in the background, and an event tells you when it is real.

## How it works

1. **The call returns a handle.** `asset_load_texture` creates a renderer texture slot
   (`renderer_texture_create`), `asset_load_material` creates a renderer material row.
   Both are usable immediately: a `Sprite` can carry the material, a `MaterialDesc` can
   name the texture. The record is `Pending`. The renderer must be up.
2. **A read is submitted** to the platform's `IoQueue`, as for every asset.
3. **The pump reaps it.** In phase `PreSim`, on the main thread, after `asset.pump_scopes`,
   `asset_render.pump` scans the completions for texture handles, decodes the PNG (the
   image module's) and hands the pixels to `renderer_texture_upload`; the upload rides the
   next frame's command buffer. `asset_render.pump_materials` runs after it, parses material
   files, loads the texture each names (step 1 again, deduplicated by path) and writes the
   renderer row.
4. **An event says so**: the asset module's `AssetReady` or `AssetFailed`. A material is
   `Ready` only when its texture is resident, so a sprite never sees a tinted checkerboard
   turn into the real image two frames apart.

**Reload.** `asset_reload` reaches both kinds (`AssetKindDesc::reload`): a texture's renderer
texture takes the new pixels, at a new size if it has one (every backend makes a new image
behind the handle and destroys the old one past the frames in flight); a material's row
takes the new look, and a new texture path loads that texture and lets the old one go. A
material made for a texture (`asset_material_for_texture`) has no file and is not read again.

**Fonts.** At startup the module gives the asset module its `AssetFontPages`: each glyph page
a font makes gets a renderer texture (`<path>@<px>:page<N>`) and a material (`font:` and the
same), alpha-blended and linearly sampled, and `asset.fonts_flush` uploads what changed in a
page through it (the rectangle where the backend can, `renderer_texture_update`). Without this
module, fonts work on the CPU: glyphs, metrics and layout, and null texture handles.

**Names.** A texture asset's renderer texture is called its path, a material file's row its
path. `asset_material_for_texture(e, texture, blend, filter, scope)` makes the material a
sprite needs for a texture asset without a file, named `<path>:<blend>` (`:linear` added for
that filter), held by the scope and holding the texture; the same texture and look is the
same record. A renderer material made by hand with no `MaterialDesc::name` is named by its
content (`material:<hash>`), the same for the same content in any order.

## Using it

**Declare the dependency.** In your `module.lua`:

```lua
module "mygame" {
  deps = { "core", "platform", "math", "render", "asset", "asset_render", "sprite", ... },
}
```

**Load at startup, keep the handles.** The renderer and the platform must be up, which
they are in any module's startup since the module list is in dependency order:

```cpp
struct Game {
    Handle rock_look;     // asset_render.material
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

`blend` is `opaque`, `alpha` or `additive`; `filter` is `nearest` (the default: pixel art) or
`linear`; `tint` is `[r, g, b]` or `[r, g, b, a]`,
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

**Poke it from the CLI** while the game runs (`--cli-stdin` or `--cli-port N`). The
assets are mounted in the CLI's filesystem:

```
ls /materials/mygame
print /materials/mygame/rock.material.json
patch /materials/mygame/rock.material.json tint 1 0.5 0.5
patch /materials/mygame/rock.material.json blend additive
patch /materials/mygame/rock.material.json texture mygame/rock2.png
ls /textures
```

A patch changes the renderer row before the next frame and nothing on disk.

## Quick reference

Header `korvid/asset_render/texture.h`:

| name | kind | what |
|---|---|---|
| `AssetState` | enum | `Pending`, `Ready`, `Failed` |
| `AssetError` | enum | `None`, `NotFound`, `Read`, `Decode`, `Upload`, `Parse` |
| `TextureAsset` | record | `path`, `state`, `error`, `texture` (render.texture), `io`, `width`, `height` |
| `AssetReady{asset}` | event | the asset turned Ready; `asset.kind()` says texture or material |
| `AssetFailed{asset, error, reload}` | event | the asset failed; it keeps its placeholder. `reload`: a reload's read could not be used, and it keeps what it had |
| `AssetReloaded{asset}` | event | the asset was made again from its changed file: the same handle, new contents |
| `asset_load_texture(e, path[, scope])` | function | handle at once; null if the stack is not up or the path is too long |
| `asset_texture(e, h)` | function | the record, or null if stale |
| `asset_error_name(err)` | function | `"not found"`, `"decode error"`, ... |

Header `korvid/asset_render/material.h`:

| name | kind | what |
|---|---|---|
| `MaterialAsset` | record | `path`, `state`, `error`, `material` (render.material), `texture_asset`, `io`, `desc`, `texture_path` |
| `asset_load_material(e, path[, scope])` | function | handle at once, row alive; holds its texture |
| `asset_material(e, h)` | function | the record, or null if stale |
| `asset_material_find(e, path)` | function | the handle a path was loaded as, or null |
| `asset_material_count(e)`, `asset_material_at(e, slot)`, `asset_material_handle_at(e, slot)` | functions | records by slot, for tools |
| `asset_material_patch(e, h, desc)` | function | record and row together; the texture follows the path |
| `asset_material_set_texture(e, h, path)` | function | loads the path and points the row at it |
| `material_parse(text, len, desc, texture_path, cap, &error, &at)` | function | the file format |
| `material_format(desc, texture_path, out, cap)` | function | the record as a file's text |

The states, errors and events (`AssetState`, `AssetError`, `AssetReady`, `AssetFailed`,
`AssetReloaded`, `asset_error_name`) are the asset module's, `korvid/asset/asset.h`, which
`texture.h` includes.

Header `korvid/asset_render/module.h`: `asset_render_module`, the record; kormake puts it in the list.

What the module registers:

| name | kind | notes |
|---|---|---|
| `asset_render.texture`, `asset_render.material` | handle kinds | `kv_handle_describe` prints path, state and size or blend; asset kinds too (`asset_kind_register`), with suffixes `.png` and `.material.json` for `scope load` |
| `asset_render.textures`, `asset_render.materials` | resources | the tables; the pumps write them |
| `asset_render.pump` | system, phase PreSim, main thread, after `asset.pump_scopes` | writes `asset_render.textures` |
| `asset_render.pump_materials` | system, phase PreSim, main thread, after `asset_render.pump` | reads `asset_render.textures`, writes `asset_render.materials` |
| `/materials` (patchable), `/textures` | CLI mounts | the records by path |

## Behaviour worth knowing

- **The placeholder is visible.** A texture that is not resident draws as the red and
  white checkerboard, tinted by the material. That is on purpose: a missing file shows
  itself. A game that wants a black frame instead waits for its `AssetReady` events.
- **Order of arrival is the disk's.** Texture reads complete in whatever order the queue
  and the kernel finish them, within a priority. A material is reported after its texture
  by construction.
- **Decode is on the main thread.** A 4096x4096 PNG costs milliseconds in `asset_render.pump`.
  The pump decodes everything that arrived this frame; a burst of large textures shows up
  as a long tick. Cooked formats (BC, ASTC) are the fix and are a later slice.
- **A sprite still naming a retired material** draws with the default look, and the dev
  build's renderer says so once per material.
- **Material edits do not touch the file.** `patch` changes the row; what you keep is
  what `dump` prints.
- **Without the renderer, loads fail.** `asset_load_texture` and `asset_load_material`
  return null when no renderer is up (a tool, a test without one); the asset module's own
  kinds do not care.
- **Limits.** 4096 texture records and 4096 material records per run; paths of 127
  characters.

## Not in this module

Cooked texture formats, mipmaps, texture atlases, formats other than PNG and the material
JSON. Reading files, scopes and reload are the asset module's; the pixels' home on the GPU
is the renderer's.
