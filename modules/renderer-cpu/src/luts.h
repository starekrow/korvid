// renderer-cpu: the tables the kernels read (korvid/renderer-cpu-raster/raster.h's CpuLuts),
// made here with the engine's math, and handed to them.
#pragma once
#include "korvid/renderer-cpu-raster/raster.h"

namespace kv { namespace rb_cpu {

// Made by the first call, which is the engine thread's, before any tile job.
const CpuLuts& cpu_luts();
// A linear channel (0 to 1, clamped) as 16-bit fixed point.
u16 cpu_linear16(f32 c);

} } // namespace kv::rb_cpu
