// Korvid renderer-d3d12: names and event markers for capture tools (PIX reads the markers
// a command list's BeginEvent writes; the ANSI form needs no PIX runtime), and GPU timing:
// timestamp and pipeline-statistics queries written while a frame records (while the
// profiler records), resolved into a readback buffer at the frame's end, read once its fence
// has passed. The queue's clock calibration pairs a GPU timestamp with the performance
// counter, which is os_ticks on Windows, so the spans land on the CPU's timeline directly.
#include "renderer_internal.h"
#include "korvid/core/profile.h"
#include "korvid/core/time.h"
#include <string.h>

namespace kv {

namespace {

constexpr u64 kStampBytes = sizeof(u64) * kGpuMarks;
constexpr u64 kStatBytes  = sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
constexpr u64 kSlotBytes  = kStampBytes + kStatBytes; // one frame's results in the readback buffer

u32 frame_slot(const Renderer* r, const FrameInFlight& f) { return (u32)(&f - r->frames); }

} // namespace

void rb_name(ID3D12Object* object, const char* name) {
    if (!object || !name) return;
    wchar_t w[128];
    if (MultiByteToWideChar(CP_UTF8, 0, name, -1, w, KV_COUNTOF(w)) > 0) object->SetName(w);
}

void rb_label_begin(Renderer* r, ID3D12GraphicsCommandList* cmd, const char* name) {
    if (r->markers) cmd->BeginEvent(1 /* ANSI text */, name, (UINT)strlen(name) + 1);
}

void rb_label_end(Renderer* r, ID3D12GraphicsCommandList* cmd) {
    if (r->markers) cmd->EndEvent();
}

bool rb_gpu_profile_create(Renderer* r) {
#if KV_PROFILE
    GpuProfiler& g = r->gpu_prof;
    u64 freq = 0;
    if (FAILED(r->queue->GetTimestampFrequency(&freq)) || freq == 0) return true; // no timing: not an error
    g.period_ns = 1e9 / (f64)freq;
    D3D12_QUERY_HEAP_DESC qd{};
    qd.Type  = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = kGpuMarks * kFramesInFlight;
    D3D_CHECK(r->device->CreateQueryHeap(&qd, IID_PPV_ARGS(&g.stamps)));
    rb_name(g.stamps, "gpu timestamps");
    qd.Type  = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
    qd.Count = kFramesInFlight;
    D3D_CHECK(r->device->CreateQueryHeap(&qd, IID_PPV_ARGS(&g.stats)));
    rb_name(g.stats, "gpu statistics");
    if (!rb_create_buffer(r, D3D12_HEAP_TYPE_READBACK, kSlotBytes * kFramesInFlight, false, g.readback, "gpu query results")) return false;
    g.timestamps = true;
    g.track = prof_track("gpu");
#else
    (void)r;
#endif
    return true;
}

void rb_gpu_profile_destroy(Renderer* r) {
    GpuProfiler& g = r->gpu_prof;
    rb_release(g.stamps);
    rb_release(g.stats);
    rb_destroy_buffer(g.readback);
    g.timestamps = false;
}

void rb_gpu_frame_begin(Renderer* r, FrameInFlight& f, ID3D12GraphicsCommandList* cmd) {
    f.gpu_stamped = f.gpu_counted = false;
    if (!r->gpu_prof.timestamps || !prof_enabled()) return;
    f.gpu_stamped = true;
    rb_gpu_mark(r, f, cmd, kGpuStart);
}

void rb_gpu_mark(Renderer* r, FrameInFlight& f, ID3D12GraphicsCommandList* cmd, GpuMark mark) {
    if (!f.gpu_stamped) return;
    cmd->EndQuery(r->gpu_prof.stamps, D3D12_QUERY_TYPE_TIMESTAMP, frame_slot(r, f) * kGpuMarks + mark);
}

void rb_gpu_stats_begin(Renderer* r, FrameInFlight& f, ID3D12GraphicsCommandList* cmd) {
    if (!f.gpu_stamped || f.gpu_counted) return;
    cmd->BeginQuery(r->gpu_prof.stats, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, frame_slot(r, f));
    f.gpu_counted = true;
}

void rb_gpu_stats_end(Renderer* r, FrameInFlight& f, ID3D12GraphicsCommandList* cmd) {
    if (f.gpu_counted) cmd->EndQuery(r->gpu_prof.stats, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, frame_slot(r, f));
}

void rb_gpu_frame_end(Renderer* r, FrameInFlight& f, ID3D12GraphicsCommandList* cmd) {
    GpuProfiler& g = r->gpu_prof;
    u32 s = frame_slot(r, f);
    if (f.gpu_stamped)
        cmd->ResolveQueryData(g.stamps, D3D12_QUERY_TYPE_TIMESTAMP, s * kGpuMarks, kGpuMarks, g.readback.resource, s * kSlotBytes);
    if (f.gpu_counted)
        cmd->ResolveQueryData(g.stats, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, s, 1, g.readback.resource, s * kSlotBytes + kStampBytes);
}

void rb_gpu_collect(Renderer* r, FrameInFlight& f) {
    GpuProfiler& g = r->gpu_prof;
    if (!f.submitted || !f.gpu_stamped) return;
    const u8* slot = (const u8*)g.readback.mapped + frame_slot(r, f) * kSlotBytes;
    u64 t[kGpuMarks];
    memcpy(t, slot, sizeof t);
    // Every so often, a GPU timestamp and the performance counter read together.
    if (!g.cal_cpu || ++g.cal_age >= 120) {
        u64 gpu = 0, cpu = 0;
        if (SUCCEEDED(r->queue->GetClockCalibration(&gpu, &cpu))) { g.cal_gpu = gpu; g.cal_cpu = cpu; g.cal_age = 0; }
    }
    f64 per_tick = (f64)os_ticks_per_second() * 1e-9 * g.period_ns; // CPU ticks per GPU tick
    auto to_cpu = [&](u64 gpu) {
        if (g.cal_cpu) return (u64)((i64)g.cal_cpu + (i64)((f64)(i64)(gpu - g.cal_gpu) * per_tick));
        return f.submit_ticks + (u64)((f64)(gpu - t[kGpuStart]) * per_tick);
    };
    f64 ms = g.period_ns * 1e-6;
    auto span = [&](GpuMark a, GpuMark b) { return (f64)(t[b] - t[a]) * ms; };
    RendererGpuStats st;
    st.valid     = true;
    st.frame_ms  = span(kGpuStart, kGpuEnd);
    st.upload_ms = span(kGpuStart, kGpuUploaded);
    st.sort_ms   = span(kGpuUploaded, kGpuSorted);
    st.draw_ms   = span(kGpuSorted, kGpuDrawn);
    u64 c[kGpuMarks];
    for (u32 i = 0; i < kGpuMarks; ++i) c[i] = to_cpu(t[i]);
    prof_track_begin(g.track, "gpu frame", c[kGpuStart]);
    prof_track_begin(g.track, "gpu upload", c[kGpuStart]);
    prof_track_end(g.track, c[kGpuUploaded]);
    prof_track_begin(g.track, "gpu sort", c[kGpuUploaded]);
    prof_track_end(g.track, c[kGpuSorted]);
    prof_track_begin(g.track, "gpu draw", c[kGpuSorted]);
    prof_track_end(g.track, c[kGpuDrawn]);
    prof_track_end(g.track, c[kGpuEnd]);
    if (f.gpu_counted) {
        D3D12_QUERY_DATA_PIPELINE_STATISTICS ps;
        memcpy(&ps, slot + kStampBytes, sizeof ps);
        st.vertices  = ps.VSInvocations;
        st.fragments = ps.PSInvocations;
        st.compute   = ps.CSInvocations;
        st.overdraw  = f.gpu_pixels ? (f32)((f64)ps.PSInvocations / (f64)f.gpu_pixels) : 0.0f;
        prof_track_counter(g.track, "gpu fragments", ps.PSInvocations, c[kGpuEnd]);
        prof_track_counter(g.track, "gpu overdraw x100", (u64)(st.overdraw * 100.0f), c[kGpuEnd]);
    }
    f.gpu_stamped = f.gpu_counted = false;
    g.last = st;
}

} // namespace kv
