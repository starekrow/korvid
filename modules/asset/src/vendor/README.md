# Vendored decoders

Single-file public-domain libraries from https://github.com/nothings/stb, unchanged.
Each is compiled as one translation unit of the asset module (`src/stb_vorbis.cc`) with
its allocation macros pointed at the engine's allocator; the rest of the module includes
only the declarations. stb_truetype, which `src/font.cc` uses for glyph atlases, is a
module of its own (`modules/truetype`).

| file | version | used by |
|---|---|---|
| `stb_vorbis.c` | 1.22 | `src/sound.cc`: Ogg Vorbis decoding for sound assets |

To upgrade, replace the file and rebuild; the wrappers set no options beyond the
allocator. License: public domain (or MIT, at your option), per the files' headers.
