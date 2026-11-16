// renderer-cpu-raster: which kernels draw (raster.h's cpu_raster_path). The widest instruction
// set this build has and this CPU runs, unless KV_CPU_ISA names one; chosen once. Built for
// the baseline: this file runs on any CPU.
#include "korvid/renderer-cpu-raster/raster.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(KV_HAS_ISA_AVX2) && defined(_MSC_VER)
#include <intrin.h>
#endif

namespace kv { namespace rb_cpu {

namespace {

const CpuRasterPath k_base = {"base", base::cpu_raster_tile, base::cpu_encode_tile};
#if defined(KV_HAS_ISA_AVX2)
const CpuRasterPath k_avx2 = {"avx2", avx2::cpu_raster_tile, avx2::cpu_encode_tile};

// AVX2 and the OS saving the YMM registers (OSXSAVE, XCR0 bits 1 and 2).
bool cpu_has_avx2() {
#if defined(_MSC_VER)
    int r[4];
    __cpuid(r, 0);
    if (r[0] < 7) return false;
    __cpuid(r, 1);
    if (!(r[2] & (1 << 27)) || !(r[2] & (1 << 28))) return false; // OSXSAVE, AVX
    if ((_xgetbv(0) & 6) != 6) return false;
    __cpuidex(r, 7, 0);
    return (r[1] & (1 << 5)) != 0;
#else
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2");
#endif
}
#endif

} // namespace

u32 cpu_raster_paths(const CpuRasterPath** out, u32 cap) {
    u32 n = 0;
    if (n < cap) out[n++] = &k_base;
#if defined(KV_HAS_ISA_AVX2)
    if (n < cap && cpu_has_avx2()) out[n++] = &k_avx2;
#endif
    return n;
}

namespace {
const CpuRasterPath* g_chosen = nullptr;
} // namespace

void cpu_raster_use(const CpuRasterPath& path) { g_chosen = &path; }

const CpuRasterPath& cpu_raster_path() {
    const CpuRasterPath*& chosen = g_chosen;
    if (chosen) return *chosen;
    const CpuRasterPath* paths[4];
    u32 n = cpu_raster_paths(paths, 4);
    chosen = paths[n - 1];
    if (const char* want = getenv("KV_CPU_ISA")) {
        bool found = false;
        for (u32 i = 0; i < n; ++i)
            if (!strcmp(paths[i]->name, want)) { chosen = paths[i]; found = true; }
        if (!found) fprintf(stderr, "KV_CPU_ISA=%s: not built or not run by this CPU; drawing with %s\n", want, chosen->name);
    }
    return *chosen;
}

} } // namespace kv::rb_cpu
