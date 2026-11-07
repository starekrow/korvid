#include "korvid/core/timer.h"
#include "korvid/core/event.h"
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
    TimerRecord r{tick, type_id, size, next_ticket++, 0, emitter};
    if (tick < next_tick + SLOTS) append(slots[tick % SLOTS], r, data);
    else append(far, r, data);
    ++pending;
    return r.ticket;
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
    pending = fired = 0;
    next_tick = 0;
}

} // namespace kv
