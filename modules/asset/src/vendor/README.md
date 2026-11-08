# Vendored decoders

Single-file public-domain libraries from https://github.com/nothings/stb, unchanged.
Each is compiled as one translation unit of the asset module (`src/stb_truetype.cc`,
`src/stb_vorbis.cc`) with its allocation macros pointed at the engine's allocator; the
rest of the module includes only the declarations.

| file | version | used by |
|---|---|---|
| `stb_truetype.h` | 1.26 | `src/font.cc`: glyph rasterization and atlas packing for font assets |
| `stb_vorbis.c` | 1.22 | `src/sound.cc`: Ogg Vorbis decoding for sound assets |

To upgrade, replace the file and rebuild; the wrappers set no options beyond the
allocator. License: public domain (or MIT, at your option), per the files' headers.
