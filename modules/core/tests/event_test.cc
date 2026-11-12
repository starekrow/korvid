#include "korvid/core/event.h"
#include "korvid/core/module.h"
#include "korvid/core/test.h"

struct Ping { kv::u32 n; };
struct Pong { kv::f32 x, y; };

static constinit const kv::EventTypeDesc k_events[] = {KV_EVENT(Ping), KV_EVENT(Pong)};

void test_event(kv::TestCtx& c) {
    using namespace kv;
    c.begin("event");

    // Ids are 0 until registered, then assigned in table order from first_id.
    KV_CHECK(c, EventType<Ping>::id == 0 && EventType<Pong>::id == 0);
    KV_CHECK(c, event_types_register(k_events, 1) == 3);
    KV_CHECK(c, EventType<Ping>::id == 1 && EventType<Pong>::id == 2);
    KV_CHECK(c, event_types_register(k_events, 1) == 0); // twice is an error, ids untouched
    KV_CHECK(c, EventType<Ping>::id == 1);

    EventBus bus;
    bus.init(3); // the engine's writer and two units

    // Nothing is in a stream before a merge.
    bus.emit(Ping{1});
    bus.emit(Ping{2});
    bus.emit(Pong{1.0f, 2.0f});
    KV_CHECK(c, bus.pending_count() == 3);
    KV_CHECK(c, bus.read<Ping>().len == 0);

    // The tick start: the engine's writer.
    bus.merge_outside(true);
    KV_CHECK(c, bus.pending_count() == 0 && bus.read<Ping>().len == 2 && bus.read<Pong>().len == 1);
    Span<const Pong> pongs = bus.read<Pong>();
    KV_CHECK(c, pongs.len == 1 && pongs[0].x == 1.0f && pongs[0].y == 2.0f);

    // A phase: unit 2 emitted before unit 1 did, and its events still come after unit 1's.
    bus.begin_phase();
    bus.writer(2).emit(Ping{4});
    bus.writer(1).emit(Ping{3});
    bus.merge(EventType<Ping>::id, 1, 2);
    Span<const Ping> pings = bus.read<Ping>();
    KV_CHECK(c, pings.len == 4 && pings[0].n == 1 && pings[1].n == 2 && pings[2].n == 3 && pings[3].n == 4);
    // A consumer's batches: numbers [from, to). An early one ends at the phase start.
    u32 n = 0;
    const Ping* all = (const Ping*)bus.batch(EventType<Ping>::id, 0, bus.streams[EventType<Ping>::id].end(), &n);
    KV_CHECK(c, n == 4 && all[3].n == 4);
    KV_CHECK(c, bus.streams[EventType<Ping>::id].phase_start == 2);

    // End of tick: what a consumer has not reached stays (here, from number 3 on); read()
    // starts over.
    u64 keep[3] = {0, 3, ~0ull};
    bus.end_tick(Span<const u64>(keep, 3));
    KV_CHECK(c, bus.read<Ping>().len == 0 && bus.streams[EventType<Ping>::id].base == 3 && bus.streams[EventType<Ping>::id].events.count() == 1);
    const Ping* last = (const Ping*)bus.batch(EventType<Ping>::id, 3, 4, &n);
    KV_CHECK(c, n == 1 && last[0].n == 4);
    KV_CHECK(c, bus.streams[EventType<Pong>::id].events.count() == 0); // nobody reads Pong
    KV_CHECK(c, bus.delivered_total == 5);

    bus.free();
    event_types_unregister(k_events);
    KV_CHECK(c, EventType<Ping>::id == 0 && EventType<Pong>::id == 0);
}
