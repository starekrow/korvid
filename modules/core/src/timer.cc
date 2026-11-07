#include "korvid/core/timer.h"
#include "korvid/core/event.h"
#include "korvid/core/log.h"
#include <string.h>

namespace kv {

namespace {

struct TimerRecord {
    u64         tick;
    u32         type;
    u32         size;
    u32         ticket;
    u32         pad;
    const char* emitter;
};

usize record_bytes(u32 size) { return sizeof(TimerRecord) + ((size + 7) & ~7u); }

void append(Array<u8>& dst, const TimerRecord& r, const void* data) {
    u8* p = dst.push_n((u32)record_bytes(r.size));
    memcpy(p, &r, sizeof r);
    memcpy(p + sizeof r, data, r.size);
    usize padded = (r.size + 7) & ~7u;
    if (padded > r.size) memset(p + sizeof r + r.size, 0, padded - r.size);
}

} // namespace

u32 TimerWheel::schedule(u64 tick, u32 type_id, u32 size, const void* data, const char* emitter) {
    if (tick < next_tick) tick = next_tick; // the past, or now: the next boundary
    if (next_ticket == 0) next_ticket = 1;  // never hand out 0
    TimerRecord r{tick, type_id, size, next_ticket++, 0, emitter};
    if (tick < next_tick + SLOTS) append(slots[tick % SLOTS], r, data);
    else append(far, r, data);
    ticks.set(r.ticket, tick);
    ++pending;
    return r.ticket;
}

namespace {

// Removes the record with `ticket` from a list, if it is there.
bool erase(Array<u8>& list, u32 ticket) {
    u8* p = list.data;
    u8* end = p + list.len;
    while (p < end) {
        const TimerRecord* r = (const TimerRecord*)p;
        usize n = record_bytes(r->size);
        if (r->ticket == ticket) {
            memmove(p, p + n, (usize)(end - p) - n);
            list.len -= (u32)n;
            return true;
        }
        p += n;
    }
    return false;
}

} // namespace

bool TimerWheel::cancel(u32 ticket) {
    const u64* tick = ticks.find(ticket);
    if (!tick) return false;
    // In its slot if it is within the horizon, else still in the far list.
    bool gone = erase(slots[*tick % SLOTS], ticket) || erase(far, ticket);
    KV_ASSERT(gone);
    ticks.remove(ticket);
    --pending;
    ++cancelled;
    return gone;
}

void TimerWheel::fire(u64 tick, EventBus& bus) {
    // The ring wrapped: file what has come within the horizon.
    if (far.len && tick % SLOTS == 0) {
        Array<u8> keep;
        keep.alloc = far.alloc;
        const u8* p = far.data;
        const u8* end = p + far.len;
        while (p < end) {
            const TimerRecord* r = (const TimerRecord*)p;
            if (r->tick < tick + SLOTS) append(slots[r->tick % SLOTS], *r, p + sizeof *r);
            else append(keep, *r, p + sizeof *r);
            p += record_bytes(r->size);
        }
        far.free();
        far = keep;
    }
    Array<u8>& slot = slots[tick % SLOTS];
    if (slot.len) {
        // Due records are emitted in schedule order; records for a later lap stay.
        u32 kept = 0;
        const u8* p = slot.data;
        const u8* end = p + slot.len;
        while (p < end) {
            const TimerRecord* r = (const TimerRecord*)p;
            usize n = record_bytes(r->size);
            if (r->tick == tick) {
                EventWriter& w = bus.writer(0);
                const char* was = w.emitter;
                w.emitter = r->emitter;
                w.emit_bytes(r->type, r->size, p + sizeof *r);
                w.emitter = was;
                ticks.remove(r->ticket);
                ++fired;
                --pending;
            } else {
                if (slot.data + kept != p) memmove(slot.data + kept, p, n);
                kept += (u32)n;
            }
            p += n;
        }
        slot.len = kept;
    }
    next_tick = tick + 1;
}

void TimerWheel::free() {
    for (Array<u8>& s : slots) s.free();
    far.free();
    ticks.free();
    pending = fired = cancelled = 0;
    next_tick = 0;
}

} // namespace kv
