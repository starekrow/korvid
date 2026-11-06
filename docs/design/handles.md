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
