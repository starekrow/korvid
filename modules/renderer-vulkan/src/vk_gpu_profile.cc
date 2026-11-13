// What the GPU spends its time on. Three tools:
//
//   - Timestamps around the frame's passes (its start, after the uploads, after the sort,
//     after the drawing, its end), read once the frame's fence has passed, never waited
//     for. They go to the profiler's "gpu" track as spans (gpu frame; gpu upload, gpu sort,
//     gpu draw inside it) at the times they happened: calibrated timestamps put the GPU's
//     clock on the CPU's, else the frame's start is taken as the moment it was submitted.
//   - A pipeline-statistics query around the sort and the drawing: vertex, fragment and
//     compute shader invocations. Fragments over the frame's pixels is its overdraw.
//   - Debug-utils names on buffers, images and pipelines and labels around passes, so a
//     capture in RenderDoc or the Radeon GPU Profiler reads "sort / scatter" and "sprite
//     rows" instead of numbers. These need no profiler and cost nothing unheard.
//
// Timing is a profiling build's (KV_PROFILE) and runs while the profiler records; a frame
// remembers whether it wrote its queries, so turning the profiler on or off mid-run never
// reads a query that was not written.
#include "renderer_internal.h"
#include "korvid/core/profile.h"
#include "korvid/core/time.h"

namespace kv {

constexpr u32 kGpuStatCount = 3; // vertex, fragment, compute invocations, in the flags' bit order

void rv_name(Renderer* r, VkObjectType type, u64 handle, const char* name) {
    if (!r->debug_utils || !handle || !name || !vk_platform_object_names()) return;
    VkDebugUtilsObjectNameInfoEXT ni{};
    ni.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    ni.objectType   = type;
    ni.objectHandle = handle;
    ni.pObjectName  = name;
    vkSetDebugUtilsObjectNameEXT(r->device, &ni);
}

void rv_label_begin(Renderer* r, VkCommandBuffer cmd, const char* name) {
    if (!r->debug_utils) return;
    VkDebugUtilsLabelEXT l{};
    l.sType      = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    l.pLabelName = name;
    vkCmdBeginDebugUtilsLabelEXT(cmd, &l);
}

void rv_label_end(Renderer* r, VkCommandBuffer cmd) {
    if (r->debug_utils) vkCmdEndDebugUtilsLabelEXT(cmd);
}

bool rv_gpu_profile_create(Renderer* r) {
#if KV_PROFILE
    GpuProfiler& g = r->gpu_prof;
    if (g.timestamps) {
        VkQueryPoolCreateInfo qi{};
        qi.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qi.queryType  = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = kGpuMarks * kFramesInFlight;
        VK_CHECK(vkCreateQueryPool(r->device, &qi, nullptr, &g.stamps));
        rv_name(r, VK_OBJECT_TYPE_QUERY_POOL, (u64)g.stamps, "gpu timestamps");
    }
    if (g.statistics) {
        VkQueryPoolCreateInfo qi{};
        qi.sType              = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qi.queryType          = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        qi.queryCount         = kFramesInFlight;
        qi.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
                                VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT |
                                VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT;
        VK_CHECK(vkCreateQueryPool(r->device, &qi, nullptr, &g.stats));
        rv_name(r, VK_OBJECT_TYPE_QUERY_POOL, (u64)g.stats, "gpu statistics");
    }
    if (g.timestamps) g.track = prof_track("gpu");
#else
    (void)r;
#endif
    return true;
}

void rv_gpu_profile_destroy(Renderer* r) {
    GpuProfiler& g = r->gpu_prof;
    if (g.stamps) vkDestroyQueryPool(r->device, g.stamps, nullptr);
    if (g.stats)  vkDestroyQueryPool(r->device, g.stats, nullptr);
    g.stamps = g.stats = VK_NULL_HANDLE;
}

namespace {

u32 frame_slot(const Renderer* r, const FrameInFlight& f) { return (u32)(&f - r->frames); }

// A GPU timestamp on the CPU's clock, in os_ticks.
u64 to_cpu(const Renderer* r, const FrameInFlight& f, u64 gpu, u64 frame_start) {
    const GpuProfiler& g = r->gpu_prof;
    f64 per_tick = (f64)os_ticks_per_second() * 1e-9 * g.period_ns; // CPU ticks per GPU tick
    if (g.calibrated) {
        i64 d = (i64)((gpu - g.cal_gpu) & g.valid_mask);
        if ((u64)d > (g.valid_mask >> 1)) d -= (i64)g.valid_mask + 1; // before the calibration
        return (u64)((i64)g.cal_cpu + (i64)((f64)d * per_tick));
    }
    return f.submit_ticks + (u64)((f64)((gpu - frame_start) & g.valid_mask) * per_tick);
}

// Every so often, a GPU timestamp and the CPU's clock read together.
void calibrate(Renderer* r) {
    GpuProfiler& g = r->gpu_prof;
    if (!g.calibrated || (g.cal_cpu && ++g.cal_age < 120)) return; // at once the first time, then every 120 frames
    VkCalibratedTimestampInfoKHR info[2]{};
    info[0].sType      = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
    info[0].timeDomain = VK_TIME_DOMAIN_DEVICE_KHR;
    info[1].sType      = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
    info[1].timeDomain = g.cpu_domain;
    u64 ts[2] = {}, deviation = 0;
    if (g.get_calibrated(r->device, 2, info, ts, &deviation) != VK_SUCCESS) return;
    g.cal_gpu = ts[0];
    g.cal_cpu = ts[1]; // CLOCK_MONOTONIC in ns, or the performance counter: os_ticks either way
    g.cal_age = 0;
}

} // namespace

void rv_gpu_frame_begin(Renderer* r, FrameInFlight& f, VkCommandBuffer cmd) {
    f.gpu_stamped = f.gpu_counted = false;
    GpuProfiler& g = r->gpu_prof;
    if (!prof_enabled()) return;
    u32 s = frame_slot(r, f);
    if (g.stamps) {
        vkCmdResetQueryPool(cmd, g.stamps, s * kGpuMarks, kGpuMarks);
        f.gpu_stamped = true;
        rv_gpu_mark(r, f, cmd, kGpuStart);
    }
    if (g.stats) vkCmdResetQueryPool(cmd, g.stats, s, 1);
}

void rv_gpu_mark(Renderer* r, FrameInFlight& f, VkCommandBuffer cmd, GpuMark mark) {
    if (!f.gpu_stamped) return;
    // When everything before it has finished: the start, the end of each pass.
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, r->gpu_prof.stamps, frame_slot(r, f) * kGpuMarks + mark);
}

void rv_gpu_stats_begin(Renderer* r, FrameInFlight& f, VkCommandBuffer cmd) {
    if (!r->gpu_prof.stats || !prof_enabled() || f.gpu_counted) return;
    vkCmdBeginQuery(cmd, r->gpu_prof.stats, frame_slot(r, f), 0);
    f.gpu_counted = true;
}

void rv_gpu_stats_end(Renderer* r, FrameInFlight& f, VkCommandBuffer cmd) {
    if (f.gpu_counted) vkCmdEndQuery(cmd, r->gpu_prof.stats, frame_slot(r, f));
}

void rv_gpu_collect(Renderer* r, FrameInFlight& f) {
    GpuProfiler& g = r->gpu_prof;
    if (!f.submitted || (!f.gpu_stamped && !f.gpu_counted)) return;
    u32 s = frame_slot(r, f);
    RendererGpuStats st;
    if (f.gpu_stamped) {
        u64 t[kGpuMarks] = {};
        // The fence has passed: the results are there, and no flag asks to wait.
        if (vkGetQueryPoolResults(r->device, g.stamps, s * kGpuMarks, kGpuMarks, sizeof t, t, sizeof(u64), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            calibrate(r);
            f64 ms = g.period_ns * 1e-6;
            auto span = [&](GpuMark a, GpuMark b) { return (f64)((t[b] - t[a]) & g.valid_mask) * ms; };
            st.valid     = true;
            st.frame_ms  = span(kGpuStart, kGpuEnd);
            st.upload_ms = span(kGpuStart, kGpuUploaded);
            st.sort_ms   = span(kGpuUploaded, kGpuSorted);
            st.draw_ms   = span(kGpuSorted, kGpuDrawn);
            u64 c[kGpuMarks];
            for (u32 i = 0; i < kGpuMarks; ++i) c[i] = to_cpu(r, f, t[i], t[kGpuStart]);
            prof_track_begin(g.track, "gpu frame", c[kGpuStart]);
            prof_track_begin(g.track, "gpu upload", c[kGpuStart]);
            prof_track_end(g.track, c[kGpuUploaded]);
            prof_track_begin(g.track, "gpu sort", c[kGpuUploaded]);
            prof_track_end(g.track, c[kGpuSorted]);
            prof_track_begin(g.track, "gpu draw", c[kGpuSorted]);
            prof_track_end(g.track, c[kGpuDrawn]);
            prof_track_end(g.track, c[kGpuEnd]);
            if (f.gpu_counted) {
                u64 v[kGpuStatCount] = {};
                if (vkGetQueryPoolResults(r->device, g.stats, s, 1, sizeof v, v, sizeof v, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                    st.vertices  = v[0];
                    st.fragments = v[1];
                    st.compute   = v[2];
                    st.overdraw  = f.gpu_pixels ? (f32)((f64)v[1] / (f64)f.gpu_pixels) : 0.0f;
                    prof_track_counter(g.track, "gpu fragments", v[1], c[kGpuEnd]);
                    prof_track_counter(g.track, "gpu overdraw x100", (u64)(st.overdraw * 100.0f), c[kGpuEnd]);
                }
            }
        }
    }
    f.gpu_stamped = f.gpu_counted = false;
    if (st.valid) g.last = st;
}

RendererGpuStats renderer_gpu_stats(const Renderer* r) { return r ? r->gpu_prof.last : RendererGpuStats{}; }

} // namespace kv
