// The frame loop's files, to each other.
#pragma once
#include "korvid/frameloop/frameloop.h"
#include "korvid/core/phase.h"

namespace kv {

// replay.cc: --record FILE and --replay FILE, taken from argv; the game's own arguments,
// kept for a recording's header; the replay's file read and its arguments applied
// (`apply` returns true when it took the value too); the two Input systems and the fresh
// sim's handler; the recording written at shutdown.
bool replay_arg(int& i, int argc, char** argv);
void replay_keep_arg(const char* arg);
bool replay_open(const char* exe, bool (*apply)(const char* arg, const char* value));
void replay_record(TickContext& ctx);
void replay_play(TickContext& ctx);
void replay_on_sim_reset(TickContext& ctx, Span<const SimReset> resets);
void replay_finish(Engine& e);

} // namespace kv
