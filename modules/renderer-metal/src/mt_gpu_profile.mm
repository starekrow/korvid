// What the GPU spends its time on, as renderer-vulkan's vk_gpu_profile.cc reports it:
//
//   - Timestamps around the frame's work, read once the frame has completed, never waited
//     for. The frame's start and end are its command buffer's; inside them, Apple GPUs
//     sample at stage boundaries only (the start and end of an encoder's work), so the marks
//     are the encoders': the upload blit's end, the sort's end, the last draw's end. They become renderer_gpu_stats (the frame, the upload, the sort,
//     the draw) and spans on the profiler's "gpu" track. Both clocks Metal samples together
//     (sampleTimestamps:gpuTimestamp:) count nanoseconds on Apple GPUs (measured: 201,058,417
//     of each over 201 ms), so the GPU's clock is put on the engine's by an offset, sampled
//     every 120 frames.
//   - Invocation counts: Apple GPUs have no statistics counter set (only the timestamp one),
//     so vertices, fragments, compute and overdraw stay zero, as the interface allows.
//   - Labels and debug groups on every pass, buffer, texture and pipeline: in every build, for
//     Xcode's GPU capture (elsewhere in the module).
//
// Timing is a profiling build's (KV_PROFILE) and runs while the profiler records; a frame
// remembers whether it carries timestamps, so turning the profiler on or off mid-run never
// reads samples that were not taken.
#include "renderer_internal.h"
#include "korvid/core/profile.h"
#include "korvid/core/time.h"

namespace kv {

bool mt_gpu_profile_create(Renderer* r) {
#if KV_PROFILE
    GpuProfiler& g = r->gpu_prof;
    if (![r->device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) {
        KV_INFO("metal", "no GPU timing: the device cannot sample counters at stage boundaries");
        return true;
    }
    id<MTLCounterSet> stamps = nil;
    for (id<MTLCounterSet> cs in r->device.counterSets)
        if ([cs.name isEqualToString:MTLCommonCounterSetTimestamp]) stamps = cs;
    if (!stamps) { KV_INFO("metal", "no GPU timing: the device has no timestamp counters"); return true; }
    @autoreleasepool {
        MTLCounterSampleBufferDescriptor* d = [[MTLCounterSampleBufferDescriptor new] autorelease];
        d.counterSet  = stamps;
        d.storageMode = MTLStorageModeShared;
        d.sampleCount = kGpuSamples * kFramesInFlight;
        d.label       = @"gpu timestamps";
        NSError* err = nil;
        g.samples = [r->device newCounterSampleBufferWithDescriptor:d error:&err];
        if (!g.samples) { KV_WARN("metal", "no GPU timing: %s", err ? err.localizedDescription.UTF8String : "?"); return true; }
    }
    g.track = prof_track("gpu");
#else
    (void)r;
#endif
    return true;
}

void mt_gpu_profile_destroy(Renderer* r) {
    [r->gpu_prof.samples release];
    r->gpu_prof.samples = nil;
}

namespace {

NSUInteger base_of(const Renderer* r, const FrameInFlight& f) { return (NSUInteger)(&f - r->frames) * kGpuSamples; }

// The GPU's clock on the engine's (os_ticks, nanoseconds): an offset, sampled together, at
// once the first time and then every 120 frames.
void calibrate(Renderer* r) {
    GpuProfiler& g = r->gpu_prof;
    if (g.calibrated && ++g.cal_age < 120) return;
    MTLTimestamp cpu = 0, gpu = 0;
    [r->device sampleTimestamps:&cpu gpuTimestamp:&gpu];
    g.gpu_to_os  = (i64)os_ticks() - (i64)gpu;
    g.calibrated = true;
    g.cal_age    = 0;
}

} // namespace

void mt_gpu_frame_begin(Renderer* r, FrameInFlight& f) {
    f.gpu_stamped = r->gpu_prof.samples && prof_enabled();
}

void mt_gpu_attach(Renderer* r, FrameInFlight& f, MTLBlitPassDescriptor* d) {
    if (!f.gpu_stamped) return;
    MTLBlitPassSampleBufferAttachmentDescriptor* a = d.sampleBufferAttachments[0];
    a.sampleBuffer              = r->gpu_prof.samples;
    a.startOfEncoderSampleIndex = base_of(r, f) + kGpuUploadStart;
    a.endOfEncoderSampleIndex   = base_of(r, f) + kGpuUploadEnd;
}

void mt_gpu_attach(Renderer* r, FrameInFlight& f, MTLComputePassDescriptor* d) {
    if (!f.gpu_stamped) return;
    MTLComputePassSampleBufferAttachmentDescriptor* a = d.sampleBufferAttachments[0];
    a.sampleBuffer              = r->gpu_prof.samples;
    a.startOfEncoderSampleIndex = base_of(r, f) + kGpuSortStart;
    a.endOfEncoderSampleIndex   = base_of(r, f) + kGpuSortEnd;
}

void mt_gpu_attach(Renderer* r, FrameInFlight& f, MTLRenderPassDescriptor* d, bool first, bool last) {
    if (!f.gpu_stamped) return;
    MTLRenderPassSampleBufferAttachmentDescriptor* a = d.sampleBufferAttachments[0];
    a.sampleBuffer               = r->gpu_prof.samples;
    a.startOfVertexSampleIndex   = first ? base_of(r, f) + kGpuDrawStart : MTLCounterDontSample;
    a.endOfVertexSampleIndex     = MTLCounterDontSample;
    a.startOfFragmentSampleIndex = MTLCounterDontSample;
    a.endOfFragmentSampleIndex   = last ? base_of(r, f) + kGpuDrawEnd : MTLCounterDontSample;
}

// The frame has completed. Its start and end are the command buffer's (GPUStartTime and
// GPUEndTime, always there: the same clock as the samples, in seconds where they count
// nanoseconds, measured). Its marks are the samples that fall inside that window: an encoder
// with nothing to do (no uploads, no sort without sprites) writes none, and an unwritten
// sample still holds what an earlier frame wrote there (measured), so a sample outside the
// window is not this frame's, and its pass is an empty span at the mark before it.
void mt_gpu_collect(Renderer* r, FrameInFlight& f) {
    GpuProfiler& g = r->gpu_prof;
    if (!f.gpu_stamped || !f.cb) return;
    f.gpu_stamped = false;
    f64 gs = f.cb.GPUStartTime, ge = f.cb.GPUEndTime;
    if (gs <= 0.0 || ge < gs) return;
    u64 start = (u64)(gs * 1e9), end = (u64)(ge * 1e9);
    NSData* data = [g.samples resolveCounterRange:NSMakeRange(base_of(r, f), kGpuSamples)];
    const MTLCounterResultTimestamp* t = data && data.length >= sizeof(MTLCounterResultTimestamp) * kGpuSamples
                                       ? (const MTLCounterResultTimestamp*)data.bytes : nullptr;
    const u64 slack = 2000; // the two clocks are one, but a double holds the seconds to a few hundred ns
    auto mark = [&](u32 i, u64 before) {
        u64 v = t ? t[i].timestamp : MTLCounterErrorValue;
        if (v == MTLCounterErrorValue || v + slack < start || v > end + slack) return before;
        return v < before ? before : v > end ? end : v;
    };
    u64 uploaded = mark(kGpuUploadEnd, start);
    u64 sorted   = mark(kGpuSortEnd, uploaded);
    u64 drawn    = mark(kGpuDrawEnd, sorted);
    calibrate(r);
    auto ms = [](u64 a, u64 b) { return (f64)(b - a) * 1e-6; };
    RendererGpuStats st;
    st.valid     = true;
    st.frame_ms  = ms(start, end);
    st.upload_ms = ms(start, uploaded);
    st.sort_ms   = ms(uploaded, sorted);
    st.draw_ms   = ms(sorted, drawn);
    auto os = [&](u64 gpu) { return (u64)((i64)gpu + g.gpu_to_os); };
    prof_track_begin(g.track, "gpu frame", os(start));
    prof_track_begin(g.track, "gpu upload", os(start));
    prof_track_end(g.track, os(uploaded));
    prof_track_begin(g.track, "gpu sort", os(uploaded));
    prof_track_end(g.track, os(sorted));
    prof_track_begin(g.track, "gpu draw", os(sorted));
    prof_track_end(g.track, os(drawn));
    prof_track_end(g.track, os(end));
    g.last = st;
}

RendererGpuStats renderer_gpu_stats(const Renderer* r) { return r ? r->gpu_prof.last : RendererGpuStats{}; }

} // namespace kv
