# inflate

Deflate decompression, compression and CRC-32 for the rest of the engine: zip entries in
packs, PNG image data both ways, compressed Tiled layers. It is miniz's `tinfl` and
`tdefl` (MIT, vendored and pinned in `src/vendor/`) behind a small Korvid API, and the
reason the engine no longer links the system's zlib.

## How it works

`src/inflate.cc` includes the vendored `miniz.c` once with everything but the
decompressor, the compressor and `mz_crc32` configured out (no zlib-style API, no archive
code, no stdio, no time, no malloc), then defines the API on top. Nothing of miniz is
visible outside that file. tinfl decodes a whole stream in one call into a buffer the
caller sized; it keeps its 11 KB of tables on the stack for the one-call forms and in
an `InflateStream` for the streaming one, and never allocates. tdefl compresses a whole
input in one call into a buffer the caller sized (`deflate_bound` always holds it), with
its 320 KB of state from the caller too: the image module's PNG writer is its user.

Each format records its uncompressed size (a zip entry's header, a PNG's width and
height, a gzip trailer, a Tiled layer's cell count), so the caller allocates the output
once and the call either fills it exactly or says why not.

## Using it

```cpp
#include "korvid/inflate/inflate.h"

u8* out = (u8*)alloc_in(a, entry.size);
InflateOut r = inflate_raw(compressed, entry.compressed_size, out, entry.size);
if (r.result != InflateResult::Ok || r.produced != entry.size)
    KV_WARN("pack", "%s: %s", name, inflate_result_name(r.result));
if (crc32(0, out, entry.size) != entry.crc) ...
```

`inflate_zlib` reads the two-byte header and checks the Adler-32 trailer (PNG's IDAT);
`inflate_gzip` skips the header's optional extra field, name, comment and header CRC and
checks the CRC-32 and length trailer. For input that arrives in pieces:

```cpp
InflateStream s;                                   // 11 KB: keep it off small stacks
inflate_stream_init(s, /*zlib=*/false, out, size);
for each piece: r = inflate_stream(s, piece, n, /*more=*/!last);   // Truncated with more = feed me
```

## Quick reference

Header `korvid/inflate/inflate.h`:

| name | kind | what |
|---|---|---|
| `InflateResult` | enum | `Ok`, `Truncated`, `Overflow`, `Corrupt`, `BadHeader`, `BadChecksum` |
| `InflateOut` | record | `result`, `consumed` (input bytes, trailer included), `produced` |
| `inflate_raw`, `inflate_zlib`, `inflate_gzip` | functions | `(src, src_len, dst, dst_cap)`: one call over the whole stream |
| `inflate_result_name(r)` | function | `"ok"`, `"truncated"`, ... |
| `crc32(crc, data, len)` | function | the zip/gzip/PNG CRC-32; start at 0, feed the result back to continue |
| `InflateStream`, `inflate_stream_init`, `inflate_stream` | record, functions | input in pieces, output in one buffer |
| `KV_INFLATE_STATE_BYTES` | constant | room for tinfl's state, checked against it at compile time |
| `deflate_zlib(src, src_len, dst, dst_cap, state, level)` | function | zlib-framed deflate (what `inflate_zlib` reads) into the caller's buffer; level 0 (stored) to 10 (smallest), 6 by default; the bytes written, or 0 if they did not fit |
| `deflate_bound(src_len)`, `KV_DEFLATE_STATE_BYTES` | function, constant | an output size that always holds; room for tdefl's state (320 KB, from an allocator) |

Not an engine module: no state, nothing registered. Link it by naming `inflate` in
`deps`.

## Behaviour worth knowing

- **The output buffer must hold everything.** tinfl runs in its non-wrapping mode: no
  32 KB ring, no copying out. A buffer one byte short is `Overflow`, with what fit
  written.
- **Garbage is safe.** Malformed input ends in `Corrupt` or `Truncated`; tinfl never reads
  past `src_len` or writes past `dst_cap`. The test suite runs it over random bytes.
- **Speed is tinfl's**, which is simple rather than fast. Packs store their entries
  uncompressed, so this is the path for PNGs and for zips a person made by hand.

## Not in this module

Streaming compression (one call over the whole input only), zstd, preset dictionaries,
and zlib's API.
