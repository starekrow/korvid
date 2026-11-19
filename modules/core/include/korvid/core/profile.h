// Korvid core: the profiler: KV_ZONE and its kin mark where the time goes, and the frame
// loop collects them every frame into a summary, a frame's tree and a trace file.
//
// Put a zone at the top of anything you want timed. It measures from that line to the end
// of the scope, on whichever thread runs it, nested inside whatever zone is open there
// already (modules/collision/src/module.cc, `broadphase`):
//
//   void broadphase(TickContext& ctx) {
//       KV_ZONE("collision.broadphase");
//       ...
//   }
//
// The scheduler already zones every system by its name, and the job pool every chunk by
// its batch's, so a zone of your own is for the pieces inside one. All the macros:
//
//   KV_ZONE("collision.broadphase");      // a span, closed at scope exit
//   KV_ZONE_WAIT("present");              // a span that is waiting, not working
//   KV_MARK("swapchain rebuilt");         // an instant
//   KV_MARK_V("scene composed", n);       // an instant with a value
//   KV_COUNTER("sprites", n);             // a value sampled over time
//
// To see them, run a game with --prof-report (the top ten zones per frame, logged with
// the frame loop's periodic report and at exit), --profile FILE (a Chrome JSON trace that
// Perfetto or the browser opens) or --hitch-ms N (the frame's tree, logged after any frame
// slower than N ms); tools take --profile and --prof-report too. Without one of those the
// profiler is off and a zone costs a load and a branch. Dev builds only: ship builds
// compile the macros to nothing.
//
// Names are static strings compared by address, so the same literal must have one
// address everywhere: gcc and clang pool literals always, MSVC with /GF (the toolchain
// sets it in every config). A name made at run time goes through name_intern first and is
// passed as its name_str (name.h), as tools/goldberg does for script zones.
//
// The hot path touches the clock and the calling thread's ring and nothing else: no
// allocation, no lock, no call into anything that could have a zone of its own (but a
// thread's first record, which claims its ring under a lock). What happens between a
// begin and an end is not constrained at all: a present, a file read, a mutex, a call
// into a library. Zones nest; the reader rebuilds the tree from begins and ends, so the
// writer tracks nothing and an end that never comes is reported, not fatal. Recording
// reads clocks and writes rings, so it never changes the sim. docs/design/profiler.md
// has the design.
#pragma once
#include "korvid/core/types.h"

namespace kv {

// 1 where the profiler is compiled in: dev builds by default (KV_DEV). A build can define
// it to 0 or 1 to choose.
#ifndef KV_PROFILE
#define KV_PROFILE KV_DEV
#endif

// ---- internal: the record format ------------------------------------------------------
// ZoneBegin and WaitBegin open a span, ZoneEnd closes the innermost one; Mark is an
// instant, Counter a sampled value, Frame and Tick the frame loop's and the tick runner's
// numbered instants.
enum class ProfKind : u8 { ZoneBegin, WaitBegin, ZoneEnd, Mark, Counter, Frame, Tick };

// 24 bytes; the kind rides in the top byte of ticks, which a nanosecond clock does not
// reach for 2 years of uptime.
struct ProfRecord {
    u64         ticks; // os_ticks in the low 56 bits, the ProfKind in the top 8
    const char* name;  // null for ZoneEnd
    u64         value; // Mark, Counter: the value; Frame, Tick: the number; zones: CPU ticks when recorded
};

// ---- rings ----------------------------------------------------------------------------
// Rings in the process: one for each thread that records, one for each track. While all
// are taken, a new thread's records are dropped.
constexpr u32 KV_PROF_MAX_RINGS = 256;

// ---- hot path: recording, on any thread -----------------------------------------------
// Whether the profiler is recording; the macros read it. Set it with prof_enable.
extern bool g_prof_on;
// True while the profiler records: for skipping work that only feeds it (the renderers'
// GPU timestamps).
inline bool prof_enabled() { return g_prof_on; }
// Turns recording on or off for the process. The frame loop turns it on for --profile,
// --prof-report and --hitch-ms, the tool module for --profile and --prof-report.
void prof_enable(bool on);
void prof_cpu_time(bool on);           // record per-thread CPU time at zone edges (a system call each)
// Names the calling thread in the stats and the trace ("main", "worker 2"); unnamed, it
// is "thread <thread_id()>".
void prof_thread_name(const char* name); // this thread's; copied, 31 characters kept

// Open a zone or a wait on this thread, and close the innermost open one: what KV_ZONE
// and KV_ZONE_WAIT do at the start and end of a scope. They record whether or not the
// profiler is on, so a caller checks prof_enabled() first. Each begin needs its end on
// the same thread.
void prof_begin(const char* name);
void prof_wait_begin(const char* name);
void prof_end();
// An instant with an optional value, and a counter's value now: what KV_MARK, KV_MARK_V
// and KV_COUNTER call once they have checked prof_enabled().
void prof_mark(const char* name, u64 value = 0);
void prof_counter(const char* name, u64 value);
// The frame and tick boundaries the summaries and the trace are cut at: nothing while
// the profiler is off.
void prof_frame(u64 frame); // the frame loop, once per frame, before prof_collect
void prof_tick(u64 tick);   // the tick runner, at the start of every tick

// ---- tracks: spans measured on another clock ------------------------------------------
// A track is a ring of its own for work timed elsewhere (the GPU, by its timestamps,
// converted to os_ticks by the caller), named like a thread and shown like one: in the
// frame stats, the thread stats, the summary and the trace. One writer at a time; spans
// in time order and nested by order (an outer's begin, its inner spans, its end), counters
// at a time of their own. A track's records are as late as its clock's results: they land
// in the frame that collects them, at the times they happened. From
// modules/renderer-vulkan/src/vk_gpu_profile.cc:
//
//   g.track = prof_track("gpu");                                    // once
//   prof_track_begin(g.track, "gpu frame", c[kGpuStart]);           // at the frame's fence
//   prof_track_begin(g.track, "gpu upload", c[kGpuStart]);
//   prof_track_end(g.track, c[kGpuUploaded]);
//   ...
//   prof_track_end(g.track, c[kGpuEnd]);
//
// A track's id is never 0; the writes below ignore track 0, so a failed claim needs no
// check at each write.
u32  prof_track(const char* name); // 0 when no ring is free; claimed for the process
// Opens a span named `name` on `track` at `ticks` (os_ticks), closes the innermost open
// one, and records a counter's value at `ticks`.
void prof_track_begin(u32 track, const char* name, u64 ticks);
void prof_track_end(u32 track, u64 ticks);
void prof_track_counter(u32 track, const char* name, u64 value, u64 ticks);

// ---- cold path: the main thread, between frames ---------------------------------------
// Drains every ring: zones into the frame summary (and the running totals), everything
// into the trace if one is open, the frame's records into the kept frame if asked. The
// frame loop calls it once a frame; a program without one calls it itself. A thread that
// writes more than 65536 records between calls loses the oldest, counted as dropped.
void prof_collect();

// One zone name's totals, over the last frame (prof_frame_stats) or since the last
// summary (prof_top).
struct ProfStat {
    const char* name;  // the zone's name, as recorded
    u32         calls; // times it closed
    u64         wall;  // ticks, summed over calls and threads
    u64         worst; // the longest single call
    u64         cpu;   // ticks, when CPU time was recorded
    bool        wait;  // a wait zone
};
// The last collected frame's zones, by wall descending; valid until the next prof_collect.
Span<const ProfStat> prof_frame_stats();

// One ring's last collected frame: a thread's, or a track's.
struct ProfThreadStat {
    const char* name;    // prof_thread_name's, "thread N", or the track's
    u64         busy;    // ticks inside the thread's outermost zones, waits among them included
    u64         waiting; // ticks in wait zones, at any depth
    u64         dropped; // records lost to ring overflow since the ring was claimed
    u32         open;    // zones still open when the frame was read
};
// One entry per ring slot, KV_PROF_MAX_RINGS of them, once prof_collect has run (empty
// before); a slot no thread has used has a null name. Valid until the next prof_collect.
Span<const ProfThreadStat> prof_thread_stats();

// Logs the top zones by average per frame since the last call, with the worst frame of
// each, then resets the running totals. Also warns for each thread that dropped records.
void prof_log_summary(u32 top);
// The same list as data, for an overlay: the zones since the last prof_log_summary, by
// wall time descending, copied into `out`. Returns how many were written; `*frames` gets
// the frame count they are summed over. Leaves the running totals alone.
u32  prof_top(Span<ProfStat> out, u32* frames);
// Keeps each collected frame's records so prof_log_frame can print the tree (--hitch-ms
// turns it on).
void prof_keep_frame(bool on);
// Logs the last collected frame as a tree, per thread, zones at or above `min_ticks`
// (--hitch-ms passes prof_ticks_from_ms(0.1)). Without prof_keep_frame(true) it warns and
// logs nothing.
void prof_log_frame(u64 min_ticks);

// A trace file in the Chrome JSON event format: every record of every thread, frames and
// ticks on a track of their own. Written at prof_collect; closed at shutdown or here.
// Opening one closes any open trace first. False, with an error logged, if `path` cannot
// be written.
bool prof_trace_open(const char* path);
// Ends the trace's JSON and closes the file; nothing if none is open.
void prof_trace_close();

// os_ticks to milliseconds, and back.
f64  prof_ms(u64 ticks);
u64  prof_ticks_from_ms(f64 ms);
// Closes the trace, frees the collector's tables, releases the rings, and turns recording
// off. The frame loop and the tool module call it at exit.
void prof_shutdown();

// ---- internal: what KV_ZONE expands to ------------------------------------------------
// RAII for the macros. Reads prof_enabled() once, at the start, so turning the profiler
// on or off inside the scope never leaves an end without its begin.
struct ProfScope {
    bool on;
    explicit ProfScope(const char* name, bool wait = false) : on(g_prof_on) {
        if (on) { if (wait) prof_wait_begin(name); else prof_begin(name); }
    }
    ~ProfScope() { if (on) prof_end(); }
    ProfScope(const ProfScope&) = delete;
    ProfScope& operator=(const ProfScope&) = delete;
};

} // namespace kv

#define KV_PROF_CAT_(a, b) a##b
#define KV_PROF_CAT(a, b) KV_PROF_CAT_(a, b)

// ---- the macros -----------------------------------------------------------------------
// KV_ZONE(name) and KV_ZONE_WAIT(name) declare an object that closes the zone at the end
// of the scope: one per line. KV_MARK, KV_MARK_V and KV_COUNTER are statements that do
// nothing while the profiler is off. In a ship build (KV_PROFILE 0) all of them compile
// away, though a value argument is still evaluated.
#if KV_PROFILE
#define KV_ZONE(name)           ::kv::ProfScope KV_PROF_CAT(kv_zone_, __LINE__)(name)
#define KV_ZONE_WAIT(name)      ::kv::ProfScope KV_PROF_CAT(kv_zone_, __LINE__)(name, true)
#define KV_MARK(name)           do { if (::kv::g_prof_on) ::kv::prof_mark(name); } while (0)
#define KV_MARK_V(name, value)  do { if (::kv::g_prof_on) ::kv::prof_mark(name, (::kv::u64)(value)); } while (0)
#define KV_COUNTER(name, value) do { if (::kv::g_prof_on) ::kv::prof_counter(name, (::kv::u64)(value)); } while (0)
#else
#define KV_ZONE(name)           ((void)0)
#define KV_ZONE_WAIT(name)      ((void)0)
#define KV_MARK(name)           ((void)0)
#define KV_MARK_V(name, value)  ((void)(value))
#define KV_COUNTER(name, value) ((void)(value))
#endif
