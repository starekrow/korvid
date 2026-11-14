// Korvid render core: the sources of the compiled-in half, in the order they build. A
// backend's src/render_core.cc includes this after its renderer_internal.h (core.h says
// how). Named .inl so that no build rule compiles it on its own.
#include "korvid/render/core/core.h"
#include "korvid/core/profile.h"

namespace kv {

RendererSurface renderer_surface() { return KV_RB_SURFACE; }

// ---- the render world -----------------------------------------------------------------------

void renderer_apply(Renderer* r, Span<const SpriteDelta> deltas) {
    if (!r) return;
    KV_ZONE("apply");
    rw_apply(r->world, deltas);
}

void rc_material_row(Renderer* r, u32 row, GpuMaterial& out) {
    const MaterialRecord& m = rw_material_at(r->world, row);
    rw_material_row(m, renderer_texture_slot(r, m.texture), out);
}

} // namespace kv
