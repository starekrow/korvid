// Korvid render core: the sources of the compiled-in half, in the order they build. A
// backend's src/render_core.cc includes this after its renderer_internal.h (core.h says
// how). Named .inl so that no build rule compiles it on its own.
#include "korvid/render/core/core.h"

namespace kv {

RendererSurface renderer_surface() { return KV_RB_SURFACE; }

} // namespace kv
