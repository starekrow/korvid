// Model values from C++, and what one costs on screen (docs/PRDs/Engine-Slice-12-PRD.md, S1
// and S2): a string past KROME_SET_TEXT arrives whole, in order with the other sets; and one
// krome_set under decoupled pacing is one frame presented, through the CPU renderer with no
// window, the frames stepped as the loop steps them (a frame only when one is wanted).
#include "korvid/krome/krome.h"
#include "../src/internal.h"
#include "korvid/asset/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/test.h"
#include "korvid/platform/module.h"
#include "korvid/platform_hid/module.h"
#include "korvid/render/renderer.h"
#include "korvid/renderer-cpu/module.h"
#include <string.h>

namespace {

using namespace kv;

const char* k_script = R"lua(
return {
  model = { title = "", n = 0 },
  on = {},
  root = col { pad = 4, gap = 4,
    text { id = "title", "{title}" },
    text { id = "n", "{n}" },
  },
}
)lua";

void frames(Engine& e, u32 n) {
    for (u32 i = 0; i < n; ++i) { platform_pump(e); engine_frame(e, 1.0 / 60.0); engine_frame_cycle(e, 1.0 / 60.0); }
}

const char* shown(Engine& e, Name sc, const char* id) {
    const KromeTree* t = krome_tree(e, sc);
    u32 n = t ? krome_find(*t, name_intern(id)) : KROME_NONE;
    return n == KROME_NONE ? "" : t->text(n);
}

} // namespace

void test_set(kv::TestCtx& c) {
    using namespace kv;
    c.begin("krome set");

    // S2: a string of any length, whole, and in the order it was set.
    {
        const ModuleDesc* const list[] = {&platform_module, &platform_hid_module, &asset_module, &krome_module};
        Engine e;
        KV_CHECK(c, engine_init(e, list));
        KromeModule& s = *krome_state(e);
        krome_viewport(e, 640, 480, 1.0f);
        krome_provide(e, "t/set.lua", k_script, strlen(k_script));
        Name sc = krome_open(e, "t/set.lua");
        frames(e, 3);
        KV_CHECK(c, krome_ready(e, sc));

        char longer[301];
        for (u32 i = 0; i < 300; ++i) longer[i] = (char)('a' + i % 26);
        longer[300] = 0;
        krome_set(e, sc, "title", longer);
        frames(e, 1);
        KV_CHECK(c, !strcmp(krome_model_text(e, sc, "title"), longer) && !strcmp(shown(e, sc, "title"), longer));
        KV_CHECK(c, s.set_texts.len == 0); // taken and freed

        // Exactly what the event holds: whole, as before.
        char exact[KROME_SET_TEXT + 1];
        memset(exact, 'x', KROME_SET_TEXT);
        exact[KROME_SET_TEXT] = 0;
        krome_set(e, sc, "title", exact);
        frames(e, 1);
        KV_CHECK(c, !strcmp(krome_model_text(e, sc, "title"), exact));

        // In order with the short ones, either way round, in one frame.
        krome_set(e, sc, "title", "short");
        krome_set(e, sc, "title", longer);
        frames(e, 1);
        KV_CHECK(c, !strcmp(krome_model_text(e, sc, "title"), longer));
        krome_set(e, sc, "title", longer);
        krome_set(e, sc, "title", "last");
        frames(e, 1);
        KV_CHECK(c, !strcmp(krome_model_text(e, sc, "title"), "last"));

        // For a screen that is not there: taken all the same, nothing left behind.
        krome_set(e, name_intern("t/nope.lua"), "title", longer);
        frames(e, 1);
        KV_CHECK(c, s.set_texts.len == 0);
        // Set and never pumped: freed at shutdown (the leak check would say).
        krome_set(e, sc, "title", longer);
        engine_shutdown(e);
    }

    // S1: one krome_set, one frame presented, under decoupled pacing.
    {
        const ModuleDesc* const list[] = {&platform_module, &platform_hid_module, &asset_module, &renderer_cpu_module, &krome_module};
        Engine e;
        KV_CHECK(c, engine_init(e, list));
        RendererDesc rd;
        rd.backend = RendererBackend::Cpu;
        rd.width = 320;
        rd.height = 200;
        rd.vsync = false;
        Renderer* r = renderer_create(e, rd);
        KV_CHECK(c, r != nullptr);
        if (r) {
            krome_viewport(e, 320, 200, 1.0f);
            krome_provide(e, "t/set.lua", k_script, strlen(k_script));
            Name sc = krome_open(e, "t/set.lua");
            const f64 dt = 1.0 / 60.0;
            u32 presents = 0;
            RenderView view;
            // A pass as decoupled pacing makes one with nothing else to do: the ticks, and a
            // frame drawn and presented only when one is wanted.
            auto pass = [&] {
                platform_pump(e);
                engine_frame(e, dt);
                if (!e.frame_wanted) return;
                e.frame_wanted = false;
                engine_frame_cycle(e, dt);
                renderer_render(r, view);
                ++presents;
            };
            // Opened and settled: frames while it loads and lays out, then none.
            for (u32 i = 0; i < 400; ++i) pass();
            KV_CHECK(c, krome_ready(e, sc));
            presents = 0;
            for (u32 i = 0; i < 30; ++i) pass();
            KV_CHECK(c, presents == 0);
            // One change: one frame, which shows it.
            krome_set(e, sc, "n", 7.0);
            for (u32 i = 0; i < 30; ++i) pass();
            KV_INFO("test", "one krome_set under decoupled pacing: %u frames presented", presents);
            KV_CHECK(c, presents == 1 && !strcmp(shown(e, sc, "n"), "7"));
            renderer_destroy(r);
        }
        engine_shutdown(e);
    }
}
