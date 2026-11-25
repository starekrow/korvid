# Handles

Nov 6, 2026. One 64-bit value for every handle the engine or a game issues.

```
63        52 51              32 31                      0
[ kind:12 ] [ generation:20 ] [ index:32 ]
```

- Zero is null. Kind 0 is reserved; for generational kinds generation 0 is reserved too,
  so a zeroed struct is never valid and a stale handle never equals a live one.
- Kind sits in the high bits, so sorting by value groups by issuer. Equality is bitwise.
- 12 bits of kind: 4095 issuers per session. 20 bits of generation: a million reuses of a
  slot before wrap; pools hand out free slots FIFO so one slot only wraps after the
  whole pool has cycled. Generation stays a fixed field; a kind without generations
  writes 1 or treats the bits as its own payload.
- The handle is never templated. `TypedHandle<T>` is optional sugar over the same bits.

## Kinds are issuers

A kind identifies the system instance that can turn its handles into pointers, not a
C++ type. Two sprite worlds are two kinds. The kind row holds a resolve function, a
context and a type key, so:

- anyone can resolve a handle without knowing who issued it (`kv_handle_resolve`);
- `kv_handle_resolve_as(h, type_key)` is manual RTTI, a pointer compare, available to
  the game through the same C API with whatever keys its own type system has;
- tools describe any handle they see (`kv_handle_describe`);
- relocation is the issuer's private business: the row holds no addresses.

Decisions:

- **Conversion function only.** A generation-array descriptor would allow validation
  without a call, at the price of a second path and an update protocol. Resolves belong
  at boundaries; systems iterate columns by index. If profiling ever disagrees, a kind
  may later publish an optional base/stride hint; the API does not preclude it.
- **Type identity is an address.** `TypeKey<T>::key` is an inline constant-initialized
  variable, unique per type across the binary, costing nothing and needing no registry.
  It is identity within a process only; anything persisted uses kind names.
- **Kind ids are never reused within a run**, so a stale handle from a dead world cannot
  resolve through a new one. The registry warns at three quarters used.
- **The registry owns the descriptor.** Registration copies it, name included, so the
  caller's struct can be a temporary; `kv_handle_kind_desc_get` returns the registered
  copy for anyone who needs to look at it later.
- **Registration is startup work, resolution is lock-free.** Rows are written once; ids
  follow registration order, hence startup order, hence deterministic run to run.
  Unregister at module shutdown, when nothing can still hold the kind in flight.

Modules may do faster things inside themselves: iterate by index, hand friends a
direct resolve function, or expose a pool outright. The registry is the common floor,
not a ceiling.

## In the engine

- `Pool<T>` is handle-compatible, not an issuer. It does not know whether its handles
  will leave the module; it stamps the kind its owner hands it (`use_kind`) and otherwise
  issues local handles with kind 0, which the registry treats as nothing. The owner
  registers the kind and decides what a resolved pointer means: a `Pool` offers
  `resolve_fn` and `describe_fn` for owners whose answer is "the item"; `SpriteWorld`
  registers `sprite.entity` and resolves to the `Transform2`, since an SoA entity has no
  single struct to point at. An owner that moves its pool updates the registered ctx
  atomically (`kv_handle_kind_set_ctx`) or keeps an indirect ctx. `valid()` checks kind as well as
  generation, so a handle from the wrong pool fails for the cost of the generation
  compare. The pool supplies resolve and describe, so every engine pool gets blind
  resolution and a readable description for free.
- A snapshot restored into a different pool keeps that pool's kind: the original's
  handles are foreign to the copy by design. In-place restore (rewind) keeps everything.
  Across sessions, persisted handles must be remapped by kind name at load; raw bits are
  only meaningful within the run that issued them.
- A friend that wants to skip the registry may index a pool's columns directly: a live
  handle's index is stable for its lifetime, and one `valid()` check covers the access.
- Packets and events carry handles unchanged: same 8 bytes as before.

## Costs

Resolve is one indirect call and stays out of inner loops. An issuer registers once per
instance it owns. The type key is per process. A session has 4095 kinds; an editor that
creates and destroys worlds all day will notice.

## OS handles are handles

Nov 24, 2026. What the OS opened for the process (a file, a socket, a pipe, a watch's
descriptor, a child process) is named by the same 64 bits as an entity or a texture.

Before this, the platform had a handle of its own, `KHandle`: the OS's value (a
descriptor, a HANDLE, a SOCKET) in 56 bits with a kind in the top byte. It kept a file's
handle from being used as a socket's. It could not say anything else, and it was a second
shape for the idea the engine already had.

The platform is now an issuer, like a ktable or the asset tables. It registers one kind
per sort of OS object (`os.file`, `os.socket`, `os.pipe`, `os.notify`, `os.process`) and
keeps one table for all of them. A slot holds the OS's value; the handle is the slot and
its generation. `Handle` is the only handle type in a public header.

What that buys, none of it visible in the code that holds the handle:

- **An OS handle goes stale.** Closing frees the slot and moves its generation on, so
  every other copy of the handle stops resolving. A read on a closed file is EBADF from
  the table, not a read on whichever file the OS gave that descriptor to next. A watch
  that still lists a closed socket leaves it out instead of polling a dead number. Raw
  descriptors cannot do this, and no caller does anything to get it.
- **Any handle can be described.** `kv_handle_describe` prints an OS handle as it prints
  an entity: `os.file#3:1 C:/game/save.kbor, opened by save, 3900 read, 0 written`. In
  dev and debug builds a slot keeps what was opened (a path, a peer, a command), which
  module opened it, when, and the bytes each way. A ship build keeps the OS's value and
  the kind, nothing else.
- **One way to ask what a handle is.** `os_handle_kind(h)` answers for any handle: the
  kind, or None for one that is not the OS's or has gone stale.

Decisions:

- **One table for the process, not per engine.** Files are opened before an engine
  exists (a tool's arguments, a test) and from threads that belong to no module (the
  queue's workers, the settings writer). The table is the platform library's own, made
  at its first use; opening and closing take its lock, resolving does not. Slots do not
  move once made, so a resolve from one thread while another opens is safe; resolving a
  handle while another thread closes that same handle is the caller's race, as it was.
- **The kinds are registered together, at first use.** Kind ids follow registration
  order; a program opens its first file at the same point in every run, so the ids are
  the same run to run for that program. Nothing persisted uses an id (saves and the
  state hash go by kind name).
- **The cost is one table lookup an I/O call**: a bounds check, a generation compare and
  a load, against a system call. The byte counters are two atomic adds, in dev and debug.
- **The OS's value is the platform's alone.** `os_handle_value` is in the module's src/;
  nothing outside it can take a handle apart, which is what makes staleness enforceable.
- **Not unified: an I/O request.** `platform.io` (a read or a write on its way through
  the queue) was already a handle kind of the queue's. It stays one: a request is not an
  OS object, and its lifetime is the queue's.

What follows from it, and is its own step: an entity can name an OS handle in a
component and be told when it stirs, as an input event the loop records (so a replay
sees the same wake on the same tick), since the handle is now something a component can
hold and the engine can check.

## Alerts

Nov 25, 2026. The step the section above left: an entity names an OS handle in a
component (`Alertable`, `korvid/platform/alert.h`) and is told, by an `Alerted` event in the
Input phase, when there is something to read on it. One system of the platform's asks the
OS, without waiting, about every Alertable's source each tick, and states those sources
as a `PlatformWatch` of its own, so a loop asleep is woken for them. A game registers no
waker and states no watch.

- **Level, not edge.** An entity is alerted every tick its handle has something unread.
  An edge would need the platform to know what the entity has read, which it does not.
- **Stale is silent.** A closed handle no longer resolves, so its entity is skipped: no
  unsubscribe, no error.
- **No existing entity type is alertable.** It is a component a game puts on a ktable of
  its own. Nothing in the engine's modules uses it.

### An alert is a wake, and a replay does not hold it

What an alerted entity reads from its handle comes from outside the run: a peer, a child,
a clock nobody recorded. A recording (`--record`) holds the named actions and the state
hashes, and nothing of this. So, for now:

- `Alertable` is a cosmetic component: it is never hashed (a slot in the OS handle table
  is not the same number from run to run), and a sim system that reads it fails
  `engine_init`.
- `Alerted` is not recorded. In a replay there is no socket; nothing is alerted.
- A game whose simulation changes because of what an alerted entity read will not
  replay. The engine keeps the component out of the sim; it cannot keep a handler from
  writing sim state with what it read. That is the author's to hold to, and is why this
  is fit for tools, UI, a debug channel, a lobby before the run starts, and not yet for
  gameplay that must replay.

**To do: an option to carry the data.** The fix is for the alert to bring what arrived
with it, and for the recording to keep that:

- the platform reads from the handle itself (up to a per-alert limit, the rest next
  tick) and the event carries the bytes; the entity never touches the handle;
- the frame loop records each such event as a new record type in the `.kvr` (the tick,
  the entity by kind name and slot, the bytes), and on replay delivers the same event on
  the same tick with no handle behind it;
- writes go the other way through the platform too, and are dropped in a replay;
- with that, the component can be sim state, and the restriction above goes.

It is opt-in per Alertable (a flag: "deliver the data"), since a tool or a UI channel has
no use for the copy. Old recordings stay readable: they hold no such records. This is the
shape of entity IPC as well: two entities, or two programs, exchanging recorded messages.
