// Korvid goldberg: the asset orchestrator's module. In the goldberg executable it is the
// tool; its verbs are CLI commands (cook, watch, explain, status).
#pragma once
#include "korvid/core/module.h"

namespace kv {

KV_MODULE_DECLARE(goldberg_module);
// The same module with no window: no Krome in the engine, so nothing that reads its events.
// What the tool loop starts (src/main.cc).
extern const ModuleDesc goldberg_headless_module;

// `goldberg ... watch ... --window` (docs/PRDs/Krome-Slice-6-PRD.md, "goldberg's window"): the
// same session, run in the Work cycle of a frame loop under a Krome window over it. True when
// the command line asks for it; goldberg_window_main is then the whole program.
bool goldberg_wants_window(int argc, char** argv);
int  goldberg_window_main(int argc, char** argv);

} // namespace kv
