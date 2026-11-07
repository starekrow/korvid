// Korvid core: the timer wheel. Scheduled events carry a target tick and wait here;
// at the first boundary of that tick they are emitted, pending, and delivered with
// everything else. A ring of slots indexed by tick covers the next 1024 ticks; anything
// further sits in an overflow list and is filed into the ring each time the ring wraps.
// Cost per tick is the slot's records plus, once per lap, a pass over the far list.
#pragma once
#include "korvid/core/array.h"
#include "korvid/core/map.h"
#include <pthread.h>

namespace kv {

struct EventBus;

struct TimerWheel {
    static constexpr u32 SLOTS = 1024;

    Array<u8> slots[SLOTS]; // records: TimerRecord header, then the event's bytes padded to 8
    Array<u8> far;          // records for ticks at or beyond the ring's horizon
    Map<u32, u64> ticks;    // ticket -> target tick, for cancel; entries leave as records fire
    u64       next_tick  = 0; // the first tick fire() has not run for
    u32       pending    = 0;
    u32       fired      = 0;
    u32       cancelled  = 0;
    u32       next_ticket = 1;
    pthread_mutex_t mu;      // systems on several threads schedule and cancel at once
    bool      ready = false;

    void init();
    // Files an event for `tick`; a tick at or before the current one means the next tick.
    // `ticket` is the caller's unique id for it, or 0 for one of the wheel's own. Returns
    // the ticket, never 0. Due records fire in ticket order, whatever order they arrived.
    u32  schedule(u64 tick, u32 type_id, u32 size, const void* data, const char* emitter, u32 ticket = 0);
    // Withdraws a scheduled event. False if it already fired, was cancelled, or never was.
    bool cancel(u32 ticket);
    // Emits every record due at `tick` into the bus, pending. Call once per tick, first.
    void fire(u64 tick, EventBus& bus);
    void free();
};

} // namespace kv
