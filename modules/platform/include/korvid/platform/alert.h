// Korvid platform: entities that are told when an OS handle stirs. An entity with an
// Alertable names a handle (a socket, a listener, a child's pipe); when there is something
// to read on it, the platform emits an Alerted naming the entity, in the Input phase, and
// a loop that was asleep is woken for it. Nothing else is needed: no waker to register,
// no watch to state, no poll.
//
//   constinit const KTableDesc k_ktables[] = {{.name = "chat.peer", .components = "Peer, Alertable", .capacity = 16}};
//   Handle peer = peers->spawn(Peer{...}, Alertable{sock.fd});
//   void on_alerted(TickContext& ctx, Span<const Alerted> alerts) {   // KV_ON(Alerted, on_alerted)
//       for (const Alerted& a : alerts) read_from(a.entity);          // then read the socket, as ever
//   }
//
// What may be relied on:
// - The alert is a level, not an edge: an entity is alerted every tick its handle has
//   something unread. Read it and the alerts stop; ignore it and they go on.
// - A handle that is closed is stale (os_handle.h): its entity is alerted no more, and
//   nothing has to be told. An Alertable whose source is null asks for nothing.
// - Alerted is emitted before anything in the Input phase that reads it runs, and an event
//   whose entity has died since is dropped by the bus, as for any event with a schema.
// - A handle this OS cannot watch while a loop sleeps (a pipe on Windows) is still
//   alerted on, a few milliseconds later than one it can.
//
// An alert is a wake and nothing more: it says "look", and what the entity then reads from
// the handle comes from outside the run. So Alertable is not simulation state (it is a
// cosmetic component: never hashed, and a sim system that reads it fails engine_init), and a
// recording (--record) holds nothing of it: a game whose sim changes because of what an
// alerted entity read will not replay. Carrying what arrived in the event, and so into
// the recording, is the step that would let it: docs/design/handles.md, "Alerts".
#pragma once
#include "korvid/platform/os_handle.h"

namespace kv {

// The component. `source` is the OS handle to hear about; set it when the entity is made,
// or later, or to null to stop. `alerts` counts the alerts so far, for whoever is curious.
struct Alertable {
    Handle source;
    u32    alerts = 0;
};

// The event: `source` has something to read, and `entity` asked to be told.
struct Alerted {
    Handle entity;
    Handle source;
};

} // namespace kv
