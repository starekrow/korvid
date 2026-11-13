# truetype

TrueType fonts, through stb_truetype (Sean Barrett's single-file library, `vendor/`,
version 1.26, unchanged). This module is where it is compiled, once for the whole engine:
its allocations go to the engine's general allocator, and its math (`floor`, `sqrt`,
`pow`, `cos`, `acos`, ...) is kv's (`modules/math`), so a glyph rasterizes to the same
pixels on every platform. Everyone else includes the declarations.

The asset module's font assets (glyph atlases) and kvscript's `kv.font` use it; a game
may too.

## Using it

```cpp
#include "korvid/truetype/truetype.h"

stbtt_fontinfo info;
if (stbtt_InitFont(&info, bytes, stbtt_GetFontOffsetForIndex(bytes, 0))) {
    f32 scale = stbtt_ScaleForMappingEmToPixels(&info, 16.0f);
    ...
}
```

The API is stb's own, documented at the top of `vendor/stb_truetype.h`. Its allocating
calls (`stbtt_MakeCodepointBitmap`, the packer) take the general allocator, attributed to
whoever is running; free what they return with `stbtt_FreeBitmap` as stb says.

## Upgrading

Replace `vendor/stb_truetype.h` with the release from https://github.com/nothings/stb
and rebuild; `src/stb_truetype.cc` sets nothing beyond the allocator, the math and its
warnings. The license is the file's own: public domain or MIT, at your choice; the
engine takes it as public domain, which asks for no notice.
