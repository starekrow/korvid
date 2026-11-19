// The asset module's mounts: /fonts, /sounds, /scopes, each printing its records
// (asset_render has /materials and /textures).
#include "korvid/asset/module.h"
#include "korvid/core/format.h"
#include "korvid/core/engine.h"
#include "asset_internal.h"
#include <string.h>

namespace kv {

namespace {

const char* state_name(AssetState st) { return st == AssetState::Ready ? "ready" : st == AssetState::Failed ? "failed" : "pending"; }

// ---- /fonts, /sounds: records by path --------------------------------------------------------

void fonts_list(CliContext& ctx, const char* dir, CliEmitFn emit, void* user) {
    AssetState_* s = asset_state(*ctx.engine);
    CliTree tree{dir, emit, user};
    for (u32 i = 0; s && i < s->fonts.len; ++i) {
        if (!s->fonts.is_alive(i)) continue;
        const FontAsset& f = s->fonts.items[i];
        char name[KV_ASSET_PATH_MAX + 16], summary[96];
        format(name, sizeof name, "%s@%g", f.path, f.px);
        u32 glyphs = 0, pages = 0;
        font_counts(f, &glyphs, &pages);
        format(summary, sizeof summary, "%s, %u glyphs in %u pages, line %.1f", state_name(f.state), glyphs, pages, f.line_height);
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
        u32 glyphs = 0, pages = 0;
        font_counts(f, &glyphs, &pages);
        ctx.print("%s at %g px: %s, %u glyphs made in %u pages of %ux%u (first slot %u), %u faces, ascent %.0f, descent %.0f, line %.0f, %u kerning pairs, %u misses, built in %.1f ms",
                  f.path, f.px, state_name(f.state), glyphs, pages, f.atlas_width, f.atlas_height, f.texture.index(),
                  f.strike ? f.strike->face_count : 0, f.ascent, f.descent, f.line_height, f.kern_count, f.misses, f.build_seconds * 1e3);
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

const CliMountDesc k_asset_mounts[3] = {
    {.path = "/fonts",     .list = fonts_list,     .describe = fonts_describe},
    {.path = "/sounds",    .list = sounds_list,    .describe = sounds_describe},
    {.path = "/scopes",    .list = asset_scopes_list, .describe = asset_scopes_describe},
};

} // namespace kv
