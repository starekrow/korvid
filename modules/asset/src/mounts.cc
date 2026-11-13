// The asset module's mounts: /materials, /textures, /fonts, /sounds. Whoever reads a
// format owns the tool: a material prints as its file would read and takes the same
// fields as the file; the rest print their records.
#include "korvid/asset/module.h"
#include "korvid/core/format.h"
#include "korvid/core/engine.h"
#include "korvid/render/renderer.h"
#include "asset_internal.h"
#include <string.h>

namespace kv {

namespace {

const char* state_name(AssetState st) { return st == AssetState::Ready ? "ready" : st == AssetState::Failed ? "failed" : "pending"; }

// ---- /materials --------------------------------------------------------------------------------

void materials_list(CliContext& ctx, const char* dir, CliEmitFn emit, void* user) {
    Engine& e = *ctx.engine;
    CliTree tree{dir, emit, user};
    for (u32 i = 0; i < asset_material_count(e); ++i) {
        const MaterialAsset* m = asset_material_at(e, i);
        if (!m) continue;
        char summary[200];
        format(summary, sizeof summary, "%s, %s, row %u, texture '%s'", state_name(m->state), blend_mode_name(m->desc.blend), m->material.index(), m->texture_path);
        if (!tree.add(m->path, false, summary)) return;
    }
}

const MaterialAsset* material_at_path(Engine& e, const char* rel, Handle* out) {
    Handle h = asset_material_find(e, rel);
    *out = h;
    return h.is_null() ? nullptr : asset_material(e, h);
}

bool materials_describe(CliContext& ctx, const char* rel) {
    Handle h;
    const MaterialAsset* m = material_at_path(*ctx.engine, rel, &h);
    if (!m) return false;
    char text[512];
    material_format(m->desc, m->texture_path, text, sizeof text);
    ctx.print("%s", text);
    ctx.print("state %s, row %u, texture slot %u%s%s", state_name(m->state), m->material.index(),
              renderer_texture_slot(renderer_get(*ctx.engine), m->desc.texture),
              m->error != AssetError::None ? ", error: " : "", m->error != AssetError::None ? asset_error_name(m->error) : "");
    return true;
}

// patch /materials/<path> texture <file> | blend <mode> | tint r g b [a] | params n...
bool materials_patch(CliContext& ctx, const char* rel, Span<const char* const> args) {
    Handle h;
    const MaterialAsset* m = material_at_path(*ctx.engine, rel, &h);
    if (!m) { ctx.fail("no material '%s' is loaded", rel); return false; }
    if (args.len < 1) { ctx.fail("patch <material> texture <file> | blend <mode> | tint r g b [a] | params n..."); return false; }
    MaterialDesc desc = m->desc;
    const char* field = args[0];
    Span<const char* const> v(args.data + 1, args.len - 1);
    if (strcmp(field, "texture") == 0) {
        if (v.len > 1) { ctx.fail("texture takes one path, or none for the default"); return false; }
        if (!asset_material_set_texture(*ctx.engine, h, v.len ? v[0] : "")) { ctx.fail("could not set the texture"); return false; }
        return materials_describe(ctx, rel);
    } else if (strcmp(field, "blend") == 0) {
        if (v.len != 1 || !blend_mode_parse(v[0], &desc.blend)) { ctx.fail("blend takes opaque, alpha or additive"); return false; }
    } else if (strcmp(field, "tint") == 0) {
        if (v.len != 3 && v.len != 4) { ctx.fail("tint takes r g b [a]"); return false; }
        f32 t[4] = {0, 0, 0, 1};
        for (u32 i = 0; i < v.len; ++i) if (!cli_arg_f32(ctx, v[i], "tint", &t[i])) return false;
        desc.tint = color_rgba(t[0], t[1], t[2], t[3]);
    } else if (strcmp(field, "params") == 0) {
        if (v.len > KV_MATERIAL_PARAMS) { ctx.fail("params takes up to %u numbers", KV_MATERIAL_PARAMS); return false; }
        for (u32 i = 0; i < v.len; ++i) if (!cli_arg_f32(ctx, v[i], "params", &desc.params[i])) return false;
    } else {
        ctx.fail("unknown field '%s': texture, blend, tint or params", field);
        return false;
    }
    if (!asset_material_patch(*ctx.engine, h, desc)) { ctx.fail("could not patch"); return false; }
    return materials_describe(ctx, rel);
}

// ---- /textures, /fonts, /sounds: records by path --------------------------------------------------

void textures_list(CliContext& ctx, const char* dir, CliEmitFn emit, void* user) {
    AssetState_* s = asset_state(*ctx.engine);
    CliTree tree{dir, emit, user};
    for (u32 i = 0; s && i < s->textures.len; ++i) {
        if (!s->textures.is_alive(i)) continue;
        const TextureAsset& t = s->textures.items[i];
        char summary[96];
        format(summary, sizeof summary, "%s, %ux%u, slot %u", state_name(t.state), t.width, t.height, t.texture.index());
        if (!tree.add(t.path, false, summary)) return;
    }
}

bool textures_describe(CliContext& ctx, const char* rel) {
    AssetState_* s = asset_state(*ctx.engine);
    for (u32 i = 0; s && i < s->textures.len; ++i) {
        if (!s->textures.is_alive(i) || strcmp(s->textures.items[i].path, rel) != 0) continue;
        const TextureAsset& t = s->textures.items[i];
        ctx.print("%s: %s, %ux%u, renderer slot %u%s%s", t.path, state_name(t.state), t.width, t.height, t.texture.index(),
                  t.error != AssetError::None ? ", error: " : "", t.error != AssetError::None ? asset_error_name(t.error) : "");
        return true;
    }
    return false;
}

void fonts_list(CliContext& ctx, const char* dir, CliEmitFn emit, void* user) {
    AssetState_* s = asset_state(*ctx.engine);
    CliTree tree{dir, emit, user};
    for (u32 i = 0; s && i < s->fonts.len; ++i) {
        if (!s->fonts.is_alive(i)) continue;
        const FontAsset& f = s->fonts.items[i];
        char name[KV_ASSET_PATH_MAX + 16], summary[96];
        format(name, sizeof name, "%s@%g", f.path, f.px);
        format(summary, sizeof summary, "%s, atlas %ux%u, line %.1f", state_name(f.state), f.atlas_width, f.atlas_height, f.line_height);
        if (!tree.add(name, false, summary)) return;
    }
}

bool fonts_describe(CliContext& ctx, const char* rel) {
    AssetState_* s = asset_state(*ctx.engine);
    for (u32 i = 0; s && i < s->fonts.len; ++i) {
        if (!s->fonts.is_alive(i)) continue;
        const FontAsset& f = s->fonts.items[i];
        char name[KV_ASSET_PATH_MAX + 16];
        format(name, sizeof name, "%s@%g", f.path, f.px);
        if (strcmp(name, rel) != 0) continue;
        ctx.print("%s at %g px: %s, %u glyphs in %u ranges, atlas %ux%u (slot %u, material row %u), ascent %.1f, descent %.1f, line %.1f, %u kerning pairs, %u misses, built in %.1f ms",
                  f.path, f.px, state_name(f.state), f.glyph_count, f.range_count, f.atlas_width, f.atlas_height, f.texture.index(),
                  f.material.index(), f.ascent, f.descent, f.line_height, f.kern_count, f.misses, f.build_seconds * 1e3);
        return true;
    }
    return false;
}

void sounds_list(CliContext& ctx, const char* dir, CliEmitFn emit, void* user) {
    AssetState_* s = asset_state(*ctx.engine);
    CliTree tree{dir, emit, user};
    for (u32 i = 0; s && i < s->sounds.len; ++i) {
        if (!s->sounds.is_alive(i)) continue;
        const SoundAsset& snd = s->sounds.items[i];
        char summary[96];
        format(summary, sizeof summary, "%s, %u Hz x%u, %.2f s", state_name(snd.state), snd.rate, snd.channels, snd.seconds());
        if (!tree.add(snd.path, false, summary)) return;
    }
}

bool sounds_describe(CliContext& ctx, const char* rel) {
    AssetState_* s = asset_state(*ctx.engine);
    for (u32 i = 0; s && i < s->sounds.len; ++i) {
        if (!s->sounds.is_alive(i) || strcmp(s->sounds.items[i].path, rel) != 0) continue;
        const SoundAsset& snd = s->sounds.items[i];
        ctx.print("%s: %s, %u Hz, %u channel%s, %u frames (%.2f s), %llu KB", snd.path, state_name(snd.state), snd.rate, snd.channels,
                  snd.channels == 1 ? "" : "s", snd.frames, snd.seconds(), (unsigned long long)((usize)snd.frames * snd.channels * 4 / 1024));
        return true;
    }
    return false;
}

} // namespace

const CliMountDesc k_asset_mounts[5] = {
    {.path = "/materials", .list = materials_list, .describe = materials_describe, .patch = materials_patch},
    {.path = "/textures",  .list = textures_list,  .describe = textures_describe},
    {.path = "/fonts",     .list = fonts_list,     .describe = fonts_describe},
    {.path = "/sounds",    .list = sounds_list,    .describe = sounds_describe},
    {.path = "/scopes",    .list = asset_scopes_list, .describe = asset_scopes_describe},
};

} // namespace kv
