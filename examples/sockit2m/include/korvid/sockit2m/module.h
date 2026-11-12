// sockit2m: the torture jobs as a module. Its systems run the file and socket jobs
// the driver (main.cc) hands it, inside the engine's tick: "sockit2m.sockets" in phase
// Input, "sockit2m.files" in PreSim, both on the main thread, with the platform module's
// I/O queue pumped once per frame by platform_pump like every other game.
#pragma once
#include "korvid/core/module.h"

namespace kv {

KV_MODULE_DECLARE(sockit2m_module);

struct FsJob;
struct SockJob;

// Which jobs the systems step this frame; null for none. Set between scenarios.
void sockit2m_set_jobs(Engine& e, FsJob* fs, SockJob* sock);
// The module's general allocator, for buffers that should count as the module's.
Allocator* sockit2m_allocator(Engine& e);

} // namespace kv
