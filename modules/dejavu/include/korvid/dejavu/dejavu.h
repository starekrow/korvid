// Korvid dejavu: DejaVu Sans, the engine's default font. A module that loads it depends on
// "dejavu", which puts the file in its program's assets, and names it by this path:
// asset_load_font(e, KV_DEJAVU_SANS, 28) (korvid/asset/font.h), or the same string in a
// document. The Bitstream Vera license is assets/DejaVuSans-LICENSE.txt.
#pragma once

namespace kv {

inline constexpr char KV_DEJAVU_SANS[] = "dejavu/DejaVuSans.ttf";

} // namespace kv
