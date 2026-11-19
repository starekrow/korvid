# dejavu

DejaVu Sans, the engine's default font, as an asset. The module has no code: it ships
`assets/DejaVuSans.ttf` with its license and one header, `korvid/dejavu/dejavu.h`, that
names the font's asset path, `KV_DEJAVU_SANS` (`"dejavu/DejaVuSans.ttf"`).

Krome and sprite_ui use it where a screen or document names no font, and frameloop's overlay
draws in it. A game that wants it for its own text adds `"dejavu"` to its module's `deps`,
which puts the file in its program's assets, and loads it like any font:

```cpp
#include "korvid/dejavu/dejavu.h"
g.font = asset_load_font(engine, KV_DEJAVU_SANS, 28.0f);
```

Fonts themselves are the asset module's (`korvid/asset/font.h`). DejaVu Sans is under the
Bitstream Vera license (`assets/DejaVuSans-LICENSE.txt`), which a shipped game carries with
its other licenses.
