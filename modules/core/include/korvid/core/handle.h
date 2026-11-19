// Korvid core: handles, the engine's references to things.
//
// Everything the engine hands out is named by a Handle rather than a pointer: an entity,
// a texture, a material, a sound, a font. A handle is 64 bits: which kind of thing (the
// issuer), which slot, and which generation of that slot. When the thing goes away its
// slot's generation moves on, so every copy of its handle stops resolving instead of
// pointing at whatever reuses the slot. Handles are plain data, so they go in components,
// events, saves and logs, and they stay valid when the storage behind them moves.
//
// What to do with one depends on who issued it. For an entity, ask its kind:
// ctx.alive(h), table->valid(h), table->get<T>(h) (null when dead). For an asset, ask the
// asset module. A handle from a delivered event with a schema is already checked live.
// To print any handle, kv_handle_describe(h.bits, buf, sizeof buf) gives
// "rocks.rock#12:3" plus what the issuer adds.
//
// Keep handles in module state and events; a handle kept in a sim column may name an
// entity that has since died, so check before use. kv_handle.h is the C side and the
// issuer's API; docs/design/handles.md has the design.
#pragma once
#include "korvid/core/kv_handle.h"
#include "korvid/core/types.h"

namespace kv {

// One handle: a kind, a generation and an index in 64 bits. Zero is null. Compare
// handles with ==; never do arithmetic on the bits.
struct Handle {
    u64 bits = 0;

    static Handle make(u16 kind, u32 gen, u32 index) { return Handle{kv_handle_make(kind, gen, index)}; }

    u16  kind()    const { return kv_handle_kind(bits); }  // which issuer: an entity kind, the texture table, ...
    u32  gen()     const { return kv_handle_gen(bits); }   // which use of the slot: changes when it is reused
    u32  index()   const { return kv_handle_index(bits); } // the slot: for an entity, its row in its kind's columns
    bool is_null() const { return bits == 0; }

    bool operator==(const Handle& o) const { return bits == o.bits; }
    bool operator!=(const Handle& o) const { return bits != o.bits; }
    bool operator<(const Handle& o)  const { return bits < o.bits; } // groups by kind
};
static_assert(sizeof(Handle) == 8, "a handle is one register");

// A unique address per type, constant-initialized, merged across the whole binary: the
// engine's stand-in for RTTI. Issuers register their pointee's key, and resolve<T>,
// set_state and the context's typed accessors check against it.
template <class T>
struct TypeKey {
    static inline const u8 key = 0;
};
template <class T>
const void* type_key() { return &TypeKey<T>::key; }

// For issuers: registers a kind of handle (kv_handle.h has the descriptor). The C call
// takes a pointer; this one takes the struct you just filled in. Copied either way.
inline u16 handle_kind_register(const kv_handle_kind_desc& desc) { return kv_handle_kind_register(&desc); }
// The issuer's name for the handle's kind ("rocks.rock", "asset.font"); "" for unknown.
inline const char* kind_name(Handle h) { return kv_handle_kind_name(h.kind()); }
// What the issuer calls a live handle (an entity's name, an asset's path; null for none),
// and the live handle of a kind it calls `name` (null for none). Names outlast runs where
// handles don't: a save writes a named handle by its name and finds it again on load.
inline const char* handle_name(Handle h) { return kv_handle_name(h.bits); }
inline Handle      handle_find(u16 kind, const char* name) { return Handle{kv_handle_find(kind, name)}; }

// The object behind a handle, from whoever issued it; null if it is null, stale or
// unknown. Lock-free from any thread.
inline void* resolve(Handle h) { return kv_handle_resolve(h.bits); }
// The same, checked: also null unless the issuer registered T's key.
//   if (const TextureAsset* t = resolve<TextureAsset>(h)) use(t->width);
template <class T>
T* resolve(Handle h) { return (T*)kv_handle_resolve_as(h.bits, type_key<T>()); }

// Optional: the same bits with the pointee type in the signature, so a function can say
// it wants a material and not any handle. get() is resolve<T>.
template <class T>
struct TypedHandle {
    Handle h;

    TypedHandle() = default;
    explicit TypedHandle(Handle raw) : h(raw) {}
    operator Handle() const { return h; }
    T*   get() const { return resolve<T>(h); }
    bool is_null() const { return h.is_null(); }
    bool operator==(const TypedHandle& o) const { return h == o.h; }
    bool operator!=(const TypedHandle& o) const { return h != o.h; }
};

} // namespace kv
