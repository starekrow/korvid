// Korvid core: handles, the C++ side of kv_handle.h. Used across every boundary instead
// of pointers. Handle is the packed value with accessors; nothing here is templated on
// the handle itself. TypeKey<T> is the identity a kind registers for its pointee type,
// and resolve<T> checks it: manual RTTI for anyone who wants it, nothing for anyone who
// does not. TypedHandle<T> is optional compile-time intent over the same bits.
#pragma once
#include "korvid/core/kv_handle.h"
#include "korvid/core/types.h"

namespace kv {

struct Handle {
    u64 bits = 0;

    static Handle make(u16 kind, u32 gen, u32 index) { return Handle{kv_handle_make(kind, gen, index)}; }

    u16  kind()    const { return kv_handle_kind(bits); }
    u32  gen()     const { return kv_handle_gen(bits); }
    u32  index()   const { return kv_handle_index(bits); }
    bool is_null() const { return bits == 0; }

    bool operator==(const Handle& o) const { return bits == o.bits; }
    bool operator!=(const Handle& o) const { return bits != o.bits; }
    bool operator<(const Handle& o)  const { return bits < o.bits; } // groups by kind
};
static_assert(sizeof(Handle) == 8, "a handle is one register");

// A unique address per type, constant-initialized, merged across the whole binary.
template <class T>
struct TypeKey {
    static inline const u8 key = 0;
};
template <class T>
const void* type_key() { return &TypeKey<T>::key; }

// The C call takes a pointer; this one takes the struct you just filled in. Copied either way.
inline u16 handle_kind_register(const kv_handle_kind_desc& desc) { return kv_handle_kind_register(&desc); }
inline const char* kind_name(Handle h) { return kv_handle_kind_name(h.kind()); }

// Blind: whoever issued it answers.
inline void* resolve(Handle h) { return kv_handle_resolve(h.bits); }
// Checked: null unless the issuer registered T's key.
template <class T>
T* resolve(Handle h) { return (T*)kv_handle_resolve_as(h.bits, type_key<T>()); }

// Optional sugar: the same bits with the pointee type in the signature.
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
