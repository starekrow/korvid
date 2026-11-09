// Korvid core: the profiler. Zones, waits, marks and counters recorded into a ring per
// thread, read between frames on the main thread into a per-frame summary, a dump of
// the last frame's tree, and a trace file Perfetto or the browser opens.
//
// The hot path touches the clock and the calling thread's ring and nothing else: no
// allocation, no lock, no call into anything that could have a zone of its own. What
// happens between a begin and an end is not constrained at all: a present, a file read,
// a mutex, a call into a library. Zones nest; the reader rebuilds the tree from begins
// and ends, so the writer tracks nothing and an end that never comes is reported, not
// fatal. Names are static strings compared by address, so the same literal must have one
// address everywhere: gcc and clang pool literals always, MSVC with /GF (the toolchain
// sets it in every config).
//
//   KV_ZONE("collision.broadphase");      // a span, closed at scope exit
//   KV_ZONE_WAIT("present");              // a span that is waiting, not working
//   KV_MARK("swapchain rebuilt");         // an instant
//   KV_MARK_V("rocks split", n);          // an instant with a value
//   KV_COUNTER("sprites", n);             // a value sampled over time
//
// Dev builds record when prof_enable(true) has been called (the frame loop does, when a
// sink is configured); ship builds compile the macros to nothing.
#pragma once
#include "korvid/core/types.h"

namespace kv {

#ifndef KV_PROFILE
#define KV_PROFILE KV_DEV
#endif

enum class ProfKind : u8 { ZoneBegin, WaitBegin, ZoneEnd, Mark, Counter, Frame, Tick };

// 24 bytes; the kind rides in the top byte of ticks, which a nanosecond clock does not
// reach for 2 years of uptime.
struct ProfRecord {
    u64         ticks;
    const char* name;  // null for ZoneEnd
    u64         value; // Mark, Counter: the value; Frame, Tick: the number; zones: CPU ticks when recorded
};

constexpr u32 KV_PROF_MAX_RINGS = 256;

// ---- hot path ---------------------------------------------------------------------------
extern bool g_prof_on;
inline bool prof_enabled() { return g_prof_on; }
void prof_enable(bool on);
void prof_cpu_time(bool on);           // record per-thread CPU time at zone edges (a system call each)
void prof_thread_name(const char* name); // this thread's; copied, 31 characters kept

void prof_begin(const char* name);
void prof_wait_begin(const char* name);
void prof_end();
void prof_mark(const char* name, u64 value = 0);
void prof_counter(const char* name, u64 value);
void prof_frame(u64 frame); // the frame loop, once per frame, before prof_collect
void prof_tick(u64 tick);   // the tick runner, at the start of every tick

// ---- cold path: the main thread, between frames ---------------------------------------
// Drains every ring: zones into the frame summary (and the running totals), everything
// into the trace if one is open, the frame's records into the kept frame if asked.
void prof_collect();

struct ProfStat {
    const char* name;
    u32         calls;
    u64         wall;  // ticks, summed over calls and threads
    u64         worst; // the longest single call
    u64         cpu;   // ticks, when CPU time was recorded
    bool        wait;
};
Span<const ProfStat> prof_frame_stats(); // the last collected frame, by wall descending

struct ProfThreadStat {
    const char* name;
    u64         busy;    // ticks in zones that are not waits
    u64         waiting; // ticks in wait zones
    u64         dropped; // records lost to ring overflow, ever
    u32         open;    // zones still open when the frame was read
};
Span<const ProfThreadStat> prof_thread_stats();

// Logs the top zones by average per frame since the last call, with the worst frame of
// each, then resets the running totals.
void prof_log_summary(u32 top);
// The same list as data, for an overlay: the zones since the last prof_log_summary, by
// wall time descending, copied into `out`. Returns how many were written; `*frames` gets
// the frame count they are summed over. Leaves the running totals alone.
u32  prof_top(Span<ProfStat> out, u32* frames);
// Keeps each collected frame's records so prof_log_frame can print the tree.
void prof_keep_frame(bool on);
// Logs the last collected frame as a tree, per thread, zones at or above `min_ticks`.
void prof_log_frame(u64 min_ticks);

// A trace file in the Chrome JSON event format: every record of every thread, frames and
// ticks on a track of their own. Written at prof_collect; closed at shutdown or here.
bool prof_trace_open(const char* path);
void prof_trace_close();

f64  prof_ms(u64 ticks);
u64  prof_ticks_from_ms(f64 ms);
void prof_shutdown(); // closes the trace, releases the rings

// RAII for the macros.
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
