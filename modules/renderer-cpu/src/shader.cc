// renderer-cpu: the game shaders' registry and their texture reads (korvid/renderer-cpu/shader.h).
// The registry is the program's: a shader's CPU forms are code linked into it, registered once
// at start-up (the game's own, and the generated modules' Slang forms).
#include "korvid/renderer-cpu/shader.h"
#include "raster.h"
#include "korvid/core/log.h"
#include <string.h>

namespace kv {

namespace {

struct Entry {
    char        name[64];
    CpuShaderFn own, generated;
    bool        skip;
};

constexpr u32 kMaxShaders = 128;
Entry g_shaders[kMaxShaders];
u32   g_count = 0;

Entry* find(const char* name, bool make) {
    if (!name || !*name) return nullptr;
    for (u32 i = 0; i < g_count; ++i)
        if (!strcmp(g_shaders[i].name, name)) return &g_shaders[i];
    if (!make) return nullptr;
    if (g_count == kMaxShaders || strlen(name) >= sizeof g_shaders[0].name) {
        KV_ERROR("cpu", "game shader '%s': no room in the CPU shader registry (%u)", name, kMaxShaders);
        return nullptr;
    }
    Entry& e = g_shaders[g_count++];
    e = Entry{};
    strcpy(e.name, name);
    return &e;
}

const rb_cpu::CpuTexture* texture_at(const CpuShaderCall& c, u32 slot) {
    const rb_cpu::CpuTexture* t = (const rb_cpu::CpuTexture*)c.textures;
    if (!t || !c.texture_count) return nullptr;
    return &t[slot < c.texture_count ? slot : 0];
}

} // namespace

void renderer_cpu_shader(const char* name, CpuShaderFn fn) {
    if (Entry* e = find(name, fn != nullptr)) e->own = fn;
}

void renderer_cpu_shader_generated(const char* name, CpuShaderFn fn) {
    if (Entry* e = find(name, fn != nullptr)) e->generated = fn;
}

void renderer_cpu_shader_skip(const char* name, bool skip) {
    if (Entry* e = find(name, skip)) e->skip = skip;
}

namespace rb_cpu {

CpuShaderFn cpu_shader_find(const char* name, bool* skipped) {
    *skipped = false;
    Entry* e = find(name, false);
    if (!e) return nullptr;
    if (e->skip) { *skipped = true; return nullptr; }
    return e->own ? e->own : e->generated;
}

} // namespace rb_cpu

void cpu_shader_sample(const CpuShaderCall& c, u32 slot, bool world, u32 sampler, f32 u, f32 v, f32 out[4]) {
    if (world) {
        if (c.world) { rb_cpu::cpu_sample_world(*(const rb_cpu::CpuWorld*)c.world, u, v, out); return; }
    } else if (const rb_cpu::CpuTexture* t = texture_at(c, slot)) {
        rb_cpu::cpu_sample_texture(*t, sampler, u, v, out);
        return;
    }
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
}

void cpu_shader_size(const CpuShaderCall& c, u32 slot, bool world, u32* width, u32* height) {
    *width = *height = 0;
    if (world) {
        if (const rb_cpu::CpuWorld* w = (const rb_cpu::CpuWorld*)c.world) { *width = w->width; *height = w->height; }
    } else if (const rb_cpu::CpuTexture* t = texture_at(c, slot)) {
        *width = t->width;
        *height = t->height;
    }
}

void cpu_shader_load(const CpuShaderCall& c, u32 slot, bool world, i32 x, i32 y, f32 out[4]) {
    // A texel's centre, read nearest.
    u32 w = 0, h = 0;
    cpu_shader_size(c, slot, world, &w, &h);
    if (!w || !h) { out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f; return; }
    f32 u = ((f32)x + 0.5f) / (f32)w, v = ((f32)y + 0.5f) / (f32)h;
    if (world) cpu_shader_sample(c, slot, true, 2, u, v, out);
    else cpu_shader_sample(c, slot, false, 0, u, v, out);
}

} // namespace kv
