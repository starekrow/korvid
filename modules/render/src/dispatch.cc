// renderer.h's functions, once: a switch over the backends the build binds, on the byte a
// renderer starts with (docs/PRDs/CPU-Renderer-PRD.md, "Two backends in one build"). kormake
// says which are bound and in what order of preference (KV_BIND_RENDER_<MODULE>=<place>);
// each backend defines the API in its own namespace (korvid/render/core/api.h). No table of
// function pointers: each case is a direct call, and with one backend bound there is one case.
#include "korvid/render/core/api.h"
#include "korvid/core/log.h"
#include <stdlib.h>
#include <string.h>

namespace kv {

// Each bound backend's declarations, in its namespace. KV_IF_<BACKEND>(M) expands M with the
// backend's namespace, id, place, kind and name when the build binds it, and to nothing when
// it does not.
#define KV_RENDER_DECLARE(ret, name, params, args) ret name params;

#ifdef KV_BIND_RENDER_RENDERER_VULKAN
namespace rb_vk { KV_RENDER_BACKEND_API KV_RENDER_API(KV_RENDER_DECLARE) }
#define KV_IF_VULKAN(M) M(rb_vk, KV_RENDER_ID_VULKAN, KV_BIND_RENDER_RENDERER_VULKAN, true, "vulkan")
#else
#define KV_IF_VULKAN(M)
#endif
#ifdef KV_BIND_RENDER_RENDERER_METAL
namespace rb_mt { KV_RENDER_BACKEND_API KV_RENDER_API(KV_RENDER_DECLARE) }
#define KV_IF_METAL(M) M(rb_mt, KV_RENDER_ID_METAL, KV_BIND_RENDER_RENDERER_METAL, true, "metal")
#else
#define KV_IF_METAL(M)
#endif
#ifdef KV_BIND_RENDER_RENDERER_D3D12
namespace rb_d3d { KV_RENDER_BACKEND_API KV_RENDER_API(KV_RENDER_DECLARE) }
#define KV_IF_D3D12(M) M(rb_d3d, KV_RENDER_ID_D3D12, KV_BIND_RENDER_RENDERER_D3D12, true, "d3d12")
#else
#define KV_IF_D3D12(M)
#endif
#ifdef KV_BIND_RENDER_RENDERER_CPU
namespace rb_cpu { KV_RENDER_BACKEND_API KV_RENDER_API(KV_RENDER_DECLARE) }
#define KV_IF_CPU(M) M(rb_cpu, KV_RENDER_ID_CPU, KV_BIND_RENDER_RENDERER_CPU, false, "cpu")
#else
#define KV_IF_CPU(M)
#endif

#define KV_BOUND(M) KV_IF_VULKAN(M) KV_IF_METAL(M) KV_IF_D3D12(M) KV_IF_CPU(M)

namespace {

// The public renderer is opaque; each backend's starts with its id. This converts to any
// backend's Renderer pointer, so one argument list serves every case.
struct AnyRenderer {
    void* p;
    template <class T> operator T*() const { return static_cast<T*>(p); }
};

u8 backend_of(const Renderer* r) { return r ? *reinterpret_cast<const u8*>(r) : 0; }

template <class T> T none() { return T{}; }
template <> void none<void>() {}

// The bound backends, in the binding's order.
struct Bound { u8 id; u32 place; bool gpu; const char* name; };
#define KV_BOUND_ROW(ns, id, place, gpu, name) Bound{id, place, gpu, name},
constexpr Bound k_bound_unsorted[] = { KV_BOUND(KV_BOUND_ROW) Bound{0, 0xFFFFFFFFu, false, ""} };
constexpr u32 k_bound_count = sizeof k_bound_unsorted / sizeof k_bound_unsorted[0] - 1;

const Bound* bound_in_order(u32 i) {
    static Bound sorted[sizeof k_bound_unsorted / sizeof k_bound_unsorted[0]];
    static bool made = false;
    if (!made) {
        for (u32 k = 0; k <= k_bound_count; ++k) sorted[k] = k_bound_unsorted[k];
        for (u32 a = 1; a < k_bound_count; ++a)
            for (u32 b = a; b > 0 && sorted[b - 1].place > sorted[b].place; --b) { Bound t = sorted[b - 1]; sorted[b - 1] = sorted[b]; sorted[b] = t; }
        made = true;
    }
    return i < k_bound_count ? &sorted[i] : nullptr;
}

const Bound* bound_by_id(u8 id) {
    for (u32 i = 0; i < k_bound_count; ++i) if (k_bound_unsorted[i].id == id) return &k_bound_unsorted[i];
    return nullptr;
}

bool probe(u8 id, const char** why) {
    *why = "not bound";
    switch (id) {
#define KV_PROBE_CASE(ns, id_, place, gpu, name) case id_: return ns::renderer_probe(why);
    KV_BOUND(KV_PROBE_CASE)
    default: return false;
    }
}

// The choice: made once a process, before the window opens.
struct Choice { bool made = false; u8 id = 0; RendererBackend want = RendererBackend::Auto; };
Choice g_choice;

bool wanted(const Bound& b, RendererBackend want) {
    return want == RendererBackend::Auto || (want == RendererBackend::Gpu) == b.gpu;
}

// KV_RENDERER: "gpu", "cpu", "auto", or a backend's name.
RendererBackend from_environment(u8* only) {
    *only = 0;
    const char* v = getenv("KV_RENDERER");
    if (!v || !*v || !strcmp(v, "auto")) return RendererBackend::Auto;
    if (!strcmp(v, "gpu")) return RendererBackend::Gpu;
    if (!strcmp(v, "cpu")) return RendererBackend::Cpu;
    for (u32 i = 0; i < k_bound_count; ++i)
        if (!strcmp(v, k_bound_unsorted[i].name)) { *only = k_bound_unsorted[i].id; return RendererBackend::Auto; }
    KV_WARN("render", "KV_RENDERER=%s names no bound backend: choosing as for auto", v);
    return RendererBackend::Auto;
}

} // namespace

bool renderer_backend_parse(const char* word, RendererBackend* out) {
    if (!word) return false;
    if (!strcmp(word, "auto")) *out = RendererBackend::Auto;
    else if (!strcmp(word, "gpu")) *out = RendererBackend::Gpu;
    else if (!strcmp(word, "cpu")) *out = RendererBackend::Cpu;
    else return false;
    return true;
}

const char* renderer_choose(RendererBackend want) {
    if (g_choice.made && (want == RendererBackend::Auto || want == g_choice.want)) {
        const Bound* b = bound_by_id(g_choice.id);
        return b ? b->name : "";
    }
    u8 only = 0;
    if (want == RendererBackend::Auto) want = from_environment(&only);
    g_choice = Choice{true, 0, want};
    for (u32 i = 0; const Bound* b = bound_in_order(i); ++i) {
        if (only ? b->id != only : !wanted(*b, want)) continue;
        const char* why = "";
        if (probe(b->id, &why)) {
            g_choice.id = b->id;
            if (k_bound_count > 1) KV_INFO("render", "the %s renderer draws", b->name);
            return b->name;
        }
        KV_INFO("render", "the %s renderer cannot draw here: %s", b->name, why && *why ? why : "it says no");
    }
    KV_ERROR("render", "no bound renderer can draw here");
    return "";
}

RendererSurface renderer_surface() {
    renderer_choose(RendererBackend::Auto);
    switch (g_choice.id) {
#define KV_SURFACE_CASE(ns, id_, place, gpu, name) case id_: return ns::renderer_surface();
    KV_BOUND(KV_SURFACE_CASE)
    default: return RendererSurface::Vulkan;
    }
}

u8 renderer_window_surface() {
    renderer_choose(RendererBackend::Auto);
    switch (g_choice.id) {
#define KV_WINDOW_CASE(ns, id_, place, gpu, name) case id_: return ns::renderer_window_surface();
    KV_BOUND(KV_WINDOW_CASE)
    default: return 0;
    }
}

Renderer* renderer_create(Engine& engine, const RendererDesc& desc) {
    if (!g_choice.made || (desc.backend != RendererBackend::Auto && desc.backend != g_choice.want)) renderer_choose(desc.backend);
    // The chosen backend first; under Auto, a GPU backend that fails gives way to the bound
    // ones after it (the CPU renderer last).
    bool past = false;
    for (u32 i = 0; const Bound* b = bound_in_order(i); ++i) {
        if (b->id == g_choice.id) past = true;
        if (!past) continue;
        if (b->id != g_choice.id) {
            if (g_choice.want != RendererBackend::Auto) break;
            const char* why = "";
            if (!probe(b->id, &why)) continue;
            KV_WARN("render", "falling back to the %s renderer", b->name);
        }
        Renderer* r = nullptr;
        switch (b->id) {
#define KV_CREATE_CASE(ns, id_, place, gpu, name) case id_: r = reinterpret_cast<Renderer*>(ns::renderer_create(engine, desc)); break;
        KV_BOUND(KV_CREATE_CASE)
        default: break;
        }
        if (r) { g_choice.id = b->id; return r; }
    }
    return nullptr;
}

Renderer* renderer_get(Engine& engine) {
    for (u32 i = 0; const Bound* b = bound_in_order(i); ++i) {
        Renderer* r = nullptr;
        switch (b->id) {
#define KV_GET_CASE(ns, id_, place, gpu, name) case id_: r = reinterpret_cast<Renderer*>(ns::renderer_get(engine)); break;
        KV_BOUND(KV_GET_CASE)
        default: break;
        }
        if (r) return r;
    }
    return nullptr;
}

// Everything that takes a renderer: to its backend.
#define KV_RENDER_DISPATCH(ret, name, params, args)                                    \
    ret name params {                                                                  \
        AnyRenderer r{const_cast<void*>(static_cast<const void*>(r_))};                \
        (void)r;                                                                       \
        switch (backend_of(r_)) {                                                      \
            KV_BOUND_CALL(name, args)                                                  \
        default: break;                                                                \
        }                                                                              \
        return none<ret>();                                                            \
    }
// A case per bound backend, calling `name` with `args`.
#ifdef KV_BIND_RENDER_RENDERER_VULKAN
#define KV_CALL_VULKAN(name, args) case KV_RENDER_ID_VULKAN: return rb_vk::name args;
#else
#define KV_CALL_VULKAN(name, args)
#endif
#ifdef KV_BIND_RENDER_RENDERER_METAL
#define KV_CALL_METAL(name, args) case KV_RENDER_ID_METAL: return rb_mt::name args;
#else
#define KV_CALL_METAL(name, args)
#endif
#ifdef KV_BIND_RENDER_RENDERER_D3D12
#define KV_CALL_D3D12(name, args) case KV_RENDER_ID_D3D12: return rb_d3d::name args;
#else
#define KV_CALL_D3D12(name, args)
#endif
#ifdef KV_BIND_RENDER_RENDERER_CPU
#define KV_CALL_CPU(name, args) case KV_RENDER_ID_CPU: return rb_cpu::name args;
#else
#define KV_CALL_CPU(name, args)
#endif
#define KV_BOUND_CALL(name, args) KV_CALL_VULKAN(name, args) KV_CALL_METAL(name, args) KV_CALL_D3D12(name, args) KV_CALL_CPU(name, args)

KV_RENDER_API(KV_RENDER_DISPATCH)

} // namespace kv
