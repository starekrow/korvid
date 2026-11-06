#include "korvid/core/event.h"
#include "korvid/core/test.h"

struct Ping { kv::u32 n; };
struct Pong { kv::f32 x, y; };

void test_event(kv::TestCtx& c) {
    using namespace kv;
    c.begin("event");

    EventBus bus;
    bus.init(2);

    // Nothing is visible before a boundary.
    bus.emit(Ping{1});
    bus.emit(Ping{2});
    bus.emit(Pong{1.0f, 2.0f});
    KV_CHECK(c, bus.pending_count() == 3);
    KV_CHECK(c, bus.read<Ping>().len == 0);
    KV_CHECK(c, bus.read<Pong>().len == 0);

    // Writer 1 emits after writer 0; merge order is writer order, then emission order.
    bus.writer(1).emit(Ping{3});
    bus.deliver();
    KV_CHECK(c, bus.pending_count() == 0);
    Span<const Ping> pings = bus.read<Ping>();
    KV_CHECK(c, pings.len == 3);
    KV_CHECK(c, pings.len == 3 && pings[0].n == 1 && pings[1].n == 2 && pings[2].n == 3);
    Span<const Pong> pongs = bus.read<Pong>();
    KV_CHECK(c, pongs.len == 1 && pongs[0].x == 1.0f && pongs[0].y == 2.0f);

    // Emitting during a phase stays pending until the next boundary; the batch accumulates.
    bus.emit(Ping{4});
    KV_CHECK(c, bus.read<Ping>().len == 3);
    bus.deliver();
    KV_CHECK(c, bus.read<Ping>().len == 4);
    KV_CHECK(c, bus.read<Ping>()[3].n == 4);

    // End of tick drops the batch; a type never emitted reads as empty.
    bus.end_tick();
    KV_CHECK(c, bus.read<Ping>().len == 0);
    KV_CHECK(c, bus.read<Pong>().len == 0);
    KV_CHECK(c, bus.delivered_total == 5);

    struct Never { kv::u8 b; };
    KV_CHECK(c, bus.read<Never>().len == 0);

    bus.free();
}
