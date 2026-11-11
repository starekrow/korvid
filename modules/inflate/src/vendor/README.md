# Vendored

`miniz.c`, `miniz.h`, `LICENSE`: miniz 3.1.2 by Rich Geldreich (MIT), the amalgamated
release from https://github.com/richgel999/miniz/releases/download/3.1.2/miniz-3.1.2.zip
(sha256 f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a), unmodified.

Only tinfl (the decompressor), tdefl (the compressor) and `mz_crc32` are compiled:
`src/inflate.cc` includes `miniz.c` with the zlib, archive, stdio, time and malloc parts
configured out.
Upgrading is replacing the two files and the hash above, then running the module's tests.
