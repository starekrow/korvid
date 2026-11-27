# image

Image files in memory: PNG both ways, JPEG decoded, and binary PPM both ways. Plain functions over
bytes, with no engine state and no I/O, so the asset module (textures) and a host tool
(`picture`) use the same code. PNG's zlib is the `inflate` module's, miniz's tinfl
and tdefl; nothing is linked from the system.

## Using it

```cpp
#include "korvid/image/jpeg.h"
#include "korvid/image/png.h"
#include "korvid/image/ppm.h"

PngImage img;
const char* why = nullptr;
if (!png_decode(bytes, size, alloc, img, &why)) KV_WARN("game", "%s: %s", path, why);
// img.rgba: width * height * 4, tightly packed, top row first; free it with `alloc`

Array<u8> file;                                   // the file is appended here
png_encode(rgb, width, height, 3, file, &why);    // 3: RGB, 4: RGBA

JpegOptions o;
o.scale = 8;                                      // an eighth: 1500 px a side comes out 188
JpegImage art;
if (jpeg_decode(bytes, size, o, alloc, art, &why)) { /* art.rgba, art.partial */ }
JpegInfo info;
jpeg_info(bytes, size, &info, &why);              // the size, before deciding to decode

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

**Decoding JPEG** (album art, KR25) reads baseline, extended and progressive Huffman
files, 8-bit: greyscale, YCbCr at any sampling, RGB, CMYK and Adobe's YCCK, with restart
markers, into RGBA8. It is written to libjpeg's arithmetic: the integer inverse DCT
(jidctint.c, and jidctred.c's 4x4, 2x2 and 1x1 for a half, a quarter and an eighth),
libjpeg-turbo's sizing of a subsampled component's blocks when scaled, its "fancy"
triangle-filter upsampling and its fixed-point colour conversion. Against Pillow on
libjpeg-turbo the pixels are identical, every case the tests have. A scaled decode does
less: at an eighth a block keeps only its DC, and a progressive file's AC scans of such a
component are not decoded at all. EXIF's orientation is applied, or reported. A hostile
file: the size is checked against `max_pixels` before any allocation, every read is
bounded, and a truncated sequential file gives the rows it had (`partial`). Not taken:
arithmetic coding, 12-bit samples, lossless and hierarchical files. Speed, one core of
this machine's Ryzen 7 7840U, a 1500x1500 4:2:0 cover of 128 KB: 20.6 ms whole, 6.0 at a
quarter, 2.8 at an eighth (libjpeg-turbo with its SIMD, through Pillow: 4.6, 1.7 and 1.4);
progressive, 25.3, 9.8 and 2.6 (8.2, 6.1 and 6.3). `tests/make_jpeg_data.py` makes the
test files and Pillow's decodes of them.

**PPM** is the binary P6 form at maxval 255, what the renderers' screenshots write. The
reader allows `#` comments between the header's fields.

## Quick reference

Header `korvid/image/png.h`:

| name | kind | what |
|---|---|---|
| `PngImage` | record | `width`, `height`, `rgba` (caller frees) |
| `png_decode(data, size, alloc, out, &error, indexes)` | function | tightly packed RGBA8, or false with a reason; with `indexes`, a palette image's indexes too (`out.index`) |
| `png_encode(pixels, width, height, channels, out, &error)` | function | appends a PNG of RGB (3) or RGBA (4) pixels to an `Array<u8>`; false for a size or channel count it cannot write |

Header `korvid/image/jpeg.h`:

| name | kind | what |
|---|---|---|
| `JpegInfo` | record | `width`, `height`, `components`, `progressive`, `orientation` (EXIF's, 1 to 8) |
| `JpegOptions` | record | `scale` (1, 2, 4, 8), `orient`, `max_pixels` |
| `JpegImage` | record | `width`, `height`, `rgba` (caller frees), `orientation` (still to apply), `partial` |
| `jpeg_info(data, size, &info, &error)` | function | the header's facts, decoding nothing |
| `jpeg_decode(data, size, opt, alloc, out, &error)` | function | RGBA8 at the scale asked, or false with a reason |

Header `korvid/image/ppm.h`:

| name | kind | what |
|---|---|---|
| `PpmImage` | record | `width`, `height`, `rgb` (caller frees) |
| `ppm_decode(data, size, alloc, out, &error)` | function | a P6 file's RGB8, or false with a reason |
| `ppm_encode(rgb, width, height, out)` | function | appends a P6 file to an `Array<u8>` |

What the module registers with the engine: nothing. It is a library.
