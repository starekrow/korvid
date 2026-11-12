# image

Image files in memory: PNG both ways and binary PPM both ways. Plain functions over
bytes, with no engine state and no I/O, so the asset module (textures) and a host tool
(`picture`) use the same code. PNG's zlib is the `inflate` module's, miniz's tinfl
and tdefl; nothing is linked from the system.

## Using it

```cpp
#include "korvid/image/png.h"
#include "korvid/image/ppm.h"

PngImage img;
const char* why = nullptr;
if (!png_decode(bytes, size, alloc, img, &why)) KV_WARN("game", "%s: %s", path, why);
// img.rgba: width * height * 4, tightly packed, top row first; free it with `alloc`

Array<u8> file;                                   // the file is appended here
png_encode(rgb, width, height, 3, file, &why);    // 3: RGB, 4: RGBA

PpmImage shot;
ppm_decode(bytes, size, alloc, shot, &why);       // shot.rgb: width * height * 3
ppm_encode(shot.rgb, shot.width, shot.height, file);
```

## How it works

**Decoding PNG** reads 8-bit gray, gray-alpha, RGB, RGBA and palette images (with tRNS),
and 1, 2 and 4-bit gray and palette ones (what pixel-art packs save), into RGBA8; not
16-bit samples or interlacing. CRCs are not verified. Temporaries come from the calling
thread's scratch and are released before it returns; a failure allocates nothing and
names its reason ("not a PNG", "corrupt image data", ...).

**Encoding PNG** writes 8-bit RGB or RGBA in one IDAT. Each row takes whichever of the
five filters leaves the smallest sum of absolute bytes, the usual heuristic, and the
stream is deflated at miniz's best. The compressor's 320 KB of state and the filtered
rows come from scratch. What it writes, the decoder reads back exactly (the tests check
every size class and both channel counts). The gallery's nine 960x540 screenshots come
to 5 to 121 KB each (the Metal references, written unfiltered by the script this
replaced, are 5 to 186 KB).

**PPM** is the binary P6 form at maxval 255, what the renderers' screenshots write. The
reader allows `#` comments between the header's fields.

## Quick reference

Header `korvid/image/png.h`:

| name | kind | what |
|---|---|---|
| `PngImage` | record | `width`, `height`, `rgba` (caller frees) |
| `png_decode(data, size, alloc, out, &error)` | function | tightly packed RGBA8, or false with a reason |
| `png_encode(pixels, width, height, channels, out, &error)` | function | appends a PNG of RGB (3) or RGBA (4) pixels to an `Array<u8>`; false for a size or channel count it cannot write |

Header `korvid/image/ppm.h`:

| name | kind | what |
|---|---|---|
| `PpmImage` | record | `width`, `height`, `rgb` (caller frees) |
| `ppm_decode(data, size, alloc, out, &error)` | function | a P6 file's RGB8, or false with a reason |
| `ppm_encode(rgb, width, height, out)` | function | appends a P6 file to an `Array<u8>` |

What the module registers with the engine: nothing. It is a library.
