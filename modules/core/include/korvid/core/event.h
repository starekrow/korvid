// Korvid core: events, the way modules tell each other that something happened.
//
// An event is a plain struct: a fact ("this bullet hit this rock"), with no behaviour.
// A module declares its event types in its ModuleDesc; any unit that declares it emits
// one can, and any unit that reads the type gets it.
//
//   struct RockHit { Handle rock, bullet; };
//   constinit const FieldDesc k_rock_hit_fields[] = {KV_FIELDS(RockHit, rock, bullet)};
//   constinit const EventTypeDesc k_events[] = {KV_EVENT_EX(RockHit, k_rock_hit_fields, 0)};
//
//   ctx.emit(RockHit{rock, bullet});                     // a unit with .emits = "RockHit"
//   void on_rock_hit(TickContext& ctx, Span<const RockHit> hits);   // a handler on it
//   for (const RockHit& h : ctx.received<RockHit>()) ...  // or a system with .reads = "RockHit"
//
// Delivery is part of the schedule. A unit that reads a type runs after every unit that
// emits it in the same phase, and sees everything emitted since it last ran: this
// phase's events, and those of the phases and ticks since. Every consumer sees every
// event once, in the same order, however the schedule ran. A read marked '^' takes only
// what was there when its phase began, and needs no order: that's how a loop of events
// between two systems goes round, once a phase.
//
// That order is within one type. Across types there is none to keep: each type is its
// own stream, a handler is handed its own type's batch, and a system that reads two
// types gets two batches with nothing to say which of their events came first. When the
// order between two facts matters, make them one event type with a field that says which
// (`kind`: started, stopped), so they share a stream. Or keep the state in a resource,
// written where it changes, and treat the events as "something changed: look".
//
// Give an event type a field table (KV_EVENT_EX) whenever it carries a handle. The bus
// then checks each batch and drops any event whose handle no longer resolves, so a
// consumer never sees an entity that died since. The table also lets the trace and the
// CLI print the event. KV_EVENT, with no table, is for events that name no entity.
//
// How it works underneath: each unit has its own writer, merged into one stream per type
// in unit order, and each consumer keeps a cursor into the stream. Writer 0 is the
// engine's, for code outside the schedule; what it gets lands at the next boundary.
// docs/design/events.md has the model.
#pragma once
#include "korvid/core/array.h"
#include "korvid/core/component.h"

namespace kv {

// ---- declaring event types ------------------------------------------------------------
// Flags for KV_EVENT_EX, ORed together.
enum EventFlags : u8 {
    EvNextTick = 1, // delivered at the first boundary of the next tick, never the same tick
    EvStaleOk  = 2, // dropped for a stale handle as a matter of course (an overlap ending with a death): no dev warning
};

// One row of a module's event table, made by KV_EVENT or KV_EVENT_EX. The type's name is
// global, like a component's: "RockHit" in every module's sets.
struct EventTypeDesc {
    const char*           name;
    u32                   size;
    u32                   align;
    u32*                  id;
    Span<const FieldDesc> fields = {}; // optional schema: stale-handle drop, trace, describe
    u8                    flags  = 0;
    const void*           defaults = nullptr; // a value-initialized E (KV_EVENT fills it): what a script leaves out

    bool next_tick() const { return (flags & EvNextTick) != 0; }
};

// The id engine_init gives event type E. Zero before init; the table entry points at it,
// and no other code sets it.
template <class E>
struct EventType {
    static_assert(__is_trivially_copyable(E), "events are plain data");
    static inline u32 id = 0;
};

// E as `E{}` makes it, its default member initializers applied: the event a script emits
// when it names none of the fields (EventTypeDesc::defaults). A char array's default is
// written in braces, {'o', 'f', 'f'}, not "off": cl (14.51) constant-initializes a member
// given a string literal as all zeros, silently, constinit or not.
template <class E>
struct EventDefault {
    static inline const E value{};
};

// Assigns ids first_id, first_id + 1, ... to the table in order. Returns the next free
// id. Fails (returns 0) on a duplicate name or a type registered twice. engine_init
// calls this; a test of the bus without an engine may call it directly.
u32  event_types_register(Span<const EventTypeDesc> types, u32 first_id);
void event_types_unregister(Span<const EventTypeDesc> types);

// ---- internal: writers and streams ------------------------------------------------------
// One type's events in one writer, packed.
struct EventBuffer {
    u32       elem_size = 0;
    Array<u8> bytes;
#if KV_DEV
    Array<const char*> emitters; // by event: who emitted it, for the trace
#endif

    u32  count() const { return elem_size ? bytes.len / elem_size : 0; }
    void clear();
    void free();
};

// One unit's pending events, by type, until its phase merges them.
struct EventWriter {
    Array<EventBuffer> by_type; // indexed by event id; slot 0 is never used
    const char*        emitter = ""; // stamped on each emit in dev builds

    template <class E>
    void emit(const E& e) {
        u32 id = EventType<E>::id;
        KV_ASSERT(id != 0); // not registered: the module list does not declare this type
        if (id == 0) return;
        emit_bytes(id, sizeof(E), &e);
    }
    void emit_bytes(u32 type_id, u32 elem_size, const void* e);
    // Everything pending in `src`, after what is pending here. The chunk merge.
    void append(const EventWriter& src);

    EventBuffer& buffer(u32 type_id, u32 elem_size);
    u32  pending_count() const;
    void clear();
    void free();
};

// One type's stream: every event merged and not yet passed by every consumer, oldest
// first, numbered from the start of the run.
struct EventStream {
    EventBuffer events;          // events.bytes holds numbers [base, end())
    u64         base        = 0;
    u64         tick_start  = 0; // the first merged this tick: what read() returns
    u64         phase_start = 0; // the first merged this phase: where early batches end
    u64         traced      = 0; // the dev trace has recorded up to here
    bool        merged      = false; // this phase's units are in

    u64 end() const { return base + events.count(); }
};

// ---- the bus ------------------------------------------------------------------------------
// Every writer and stream, owned by the Engine (Engine::events). Units never touch it:
// they emit and read through the context. Code outside the schedule (startup, a CLI
// command, a test) uses emit and read below.
struct EventBus {
    Array<EventWriter> writers;   // 0: the engine's; unit u: writers[u]
    Array<EventStream> streams;   // by type id
    EventWriter        held;      // next-tick events, for the next tick's start
    Span<const EventTypeDesc* const> types; // by id, set by the engine; empty means no schemas
    u64                delivered_total = 0; // merged, ever
    u64                dropped_stale   = 0; // dropped from a batch for a stale handle, ever
    Array<u8>          stale_warned;        // by type: dev builds warn the first time a type is dropped
    Array<u8>              held_warned;  // dev: by type, a consumer that does not run was named for holding it
    u32                main_thread     = 0; // the thread that may write the engine's writer during a tick

    void init(u32 writer_count = 1);
    void free();

    EventWriter& writer(u32 i) { return writers[i]; }
    void set_emitter(const char* name) { writers[0].emitter = name; }

    // Emits from outside the schedule, into the engine's writer: startup, a CLI command,
    // a platform callback. Also allowed from a unit on the main thread, for a fact that is
    // not the unit's to order (a library reporting as it goes). It lands at the next
    // boundary, so no consumer in the current phase sees it.
    //   e.events.emit(PadButtonDown{b, slot});   // the touchpad, from its platform callback
    template <class E>
    void emit(const E& e) { check_outside(); writers[0].emit(e); }
    void check_outside() const;
    void check_read() const; // dev builds: not from inside a unit, which has its batch

    // A phase begins: early batches end at what is merged now.
    void begin_phase();
    // Units [first, last] (one phase's, in unit order) into the streams: one type, or
    // every type. Next-tick types go to `held`.
    void merge(u32 type_id, u32 first, u32 last);
    void merge_all(u32 first, u32 last);
    // The engine's writer; at a tick's start the held next-tick events go first.
    void merge_outside(bool tick_start);
    // Numbers [from, to) of one type, less any naming the dead: in place, or copied into
    // the caller's scratch when something was dropped. `count` is what is left.
    const void* batch(u32 type_id, u64 from, u64 to, u32* count);
    // End of tick: drop what every consumer has passed (`keep_from` by type: the lowest
    // cursor, or the stream's end for a type nobody reads). read() starts over.
    void end_tick(Span<const u64> keep_from);
    // After a cycle outside the tick: drops what every consumer has passed, but nothing
    // the tick's own window (tick_start, what read() returns) still holds.
    void trim(Span<const u64> keep_from);
    // Drop everything merged and pending: a fresh sim. Cursors behind the new base read
    // from it.
    void drop_all();

    // The events of a type merged so far in the running tick, for engine code between
    // phases and tests that drive the bus by hand. The window starts over at each tick's
    // end, so after engine_frame returns it is empty: to watch a type from a test, give
    // the test a module with a handler on it. Dev builds refuse it from inside a unit,
    // which reads its batch through the context.
    template <class E>
    Span<const E> read() const {
        check_read();
        u32 id = EventType<E>::id;
        KV_ASSERT(id != 0);
        if (id == 0 || id >= streams.len) return Span<const E>();
        const EventStream& s = streams[id];
        u64 skip = s.tick_start > s.base ? s.tick_start - s.base : 0;
        return Span<const E>((const E*)s.events.bytes.data + skip, (u32)(s.events.count() - skip));
    }

    u32 pending_count() const; // in the writers and held
};

} // namespace kv

// Rows of a module's event table: KV_EVENT(WindowResized) for an event with no handles,
// or with a field table and EventFlags:
//   KV_EVENT_EX(RockHit, k_rock_hit_fields, 0)
//   KV_EVENT_EX(ScoreChanged, k_score_fields, EvNextTick)
#define KV_EVENT(E) {#E, (::kv::u32)sizeof(E), (::kv::u32)alignof(E), &::kv::EventType<E>::id, {}, 0, &::kv::EventDefault<E>::value}
#define KV_EVENT_EX(E, fields, flags) \
    {#E, (::kv::u32)sizeof(E), (::kv::u32)alignof(E), &::kv::EventType<E>::id, fields, (::kv::u8)(flags), &::kv::EventDefault<E>::value}
