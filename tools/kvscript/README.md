# kvscript

The script runner, `bin/kvscript`: Lua 5.5 with the engine's modules as libraries, for the
tools that make a game's assets: cut sprites from a sheet, fit and composite them, draw
a few letters, read a zip of a pack, write PNGs. Before it they were Python scripts that
needed Pillow; now they need nothing the engine does not build. It is an engine tool, as
`kpack` and `picture` are (see `modules/tool`): built for the host, its one command
`kvscript run`, which `kormake run` builds and calls.

    kormake run examples/arcfire/tools/cut_art.lua ~/code/asset-packs
    kvscript run SCRIPT.lua [ARGS...]           # the same, once it is built

The script gets `arg` (arg[0] the script, then its arguments, which also come as `...`)
and runs from the current directory. It ends with 0, or the number it returns; an error
ends it with 1 and prints the message and a traceback.

## What a script has

Lua's own libraries, all of them, and `kv`:

| name | what |
|---|---|
| `kv.image.new(w, h [, r, g, b, a])` | a picture, clear unless a color is given |
| `kv.image.load(path)`, `kv.image.decode(bytes)` | a PNG (any the image module reads) or a binary PPM |
| `img.width`, `img.height` | its size |
| `img:get(x, y)`, `img:set(x, y, r, g, b [, a])`, `img:alpha(x, y)`, `img:channel(n)` | pixels; `channel` is one channel (1..4) of all of them as a string, for scans |
| `img:copy()`, `img:crop(x0, y0, x1, y1)` | new pictures; a crop past the edge is clear there |
| `img:flip("vertical" or "horizontal")`, `img:rotate(degrees [, filter, expand])`, `img:resize(w, h [, filter])` | new pictures; rotate is counter-clockwise, filter `nearest` (default), `bilinear` or `bicubic`; resize's `lanczos`, `bicubic` (default), `bilinear`, `box` or `nearest` |
| `img:composite(src [, x, y])`, `img:paste(src [, x, y])` | src over this picture, or in place of it, at (x, y); return `img` |
| `img:bbox([threshold])` | x0, y0, x1, y1 of the pixels whose alpha is over threshold (0), or nil |
| `img:key(r, g, b)`, `img:scale(kr, kg, kb [, ka])` | new pictures: that color made clear; each channel times its factor, truncated |
| `img:save(path)`, `img:encode()` | PNG, to a file or as a string |
| `kv.font.load(path, px)`, `font:draw(img, text, x, y, r, g, b, a [, anchor])`, `font:measure(text)` | TrueType (stb_truetype) at px to the em; anchor `l`/`m`/`r` then `a`/`m`/`s`/`d` |
| `kv.zip.open(path)`, `z:names()`, `z:read(name)`, `z:has(name)` | a zip read whole; names in the archive's order; entries stored or deflated, CRCs checked |
| `kv.fs.read`, `write`, `exists`, `is_file`, `is_dir`, `mkdir`, `list`, `remove`, `remove_tree`, `temp_dir` | files through the platform (UTF-8 paths on every OS); `read` and `write` return nil and a message on failure, `list` every file under a directory |

Use `kv.fs` rather than Lua's `io` for files: `io` is the C library's, which on Windows
does not take UTF-8 paths.

## Behaviour worth knowing

- **Pillow's results, to the pixel.** The image operations do what Pillow's do where a
  tool ported from it should come out the same: `composite` is `alpha_composite`'s integer
  blend; `resize` is Pillow's two fixed-point passes on premultiplied pixels with its
  filters; `rotate` with `bicubic` or `bilinear` is its affine transform, each pixel's
  source computed as its generic transform computes it, on premultiplied pixels; `crop`,
  `key`, `scale` and `bbox` are Pillow's crop, a color key, `point(lambda v: int(v * k))`
  and `getbbox`; `font:draw` places and blends text as `ImageDraw.text` does into RGBA.
  arcfire's `cut_art.lua`, ported from `cut_art.py`, makes all twenty of its pictures
  identical to Pillow's in every channel. Only the PNG bytes differ: the image module's
  encoder is not Pillow's.
- **The engine's math.** Lua's `math` library and its `%`, `^` and `//` are kv's
  (`modules/math`), so a script computes the same on every platform, as the engine does.
- **Errors are Lua's.** A bad argument or a file that cannot be read raises; a script
  that wants to go on uses `pcall`, or `kv.fs.read`'s nil.
- **Pictures are Lua values.** Their pixels live in the Lua heap and go with the
  collector; nothing needs freeing.

## How it is built

`src/lua.cc` compiles kormake's own Lua 5.5.1 (`tools/kormake/vendor/lua`, unchanged) as
C++ in one file, as Lua's `onelua.c` does, with `longjmp` for its errors (the engine has
no exceptions) and its math names defined to kv's after `<math.h>`. Because of that the
bindings keep nothing with a destructor alive across a call that can raise, and every
buffer that must outlive one is a userdata. `src/image.cc` is `kv.image`, `src/font.cc`
`kv.font` (the asset module's stb_truetype, compiled once more with `STBTT_STATIC`),
`src/kvscript.cc` the module, the command, `kv.zip` and `kv.fs`. The module is not a unity
build: Lua's sources define short macros that must stay in their file.
