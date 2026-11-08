// Korvid core: entity kinds. A kind is a named, fixed set of components plus a capacity,
// declared by a module as data; the engine turns each one into a Table: a generational
// slot pool for liveness plus one SoA column per component, on reserved pages committed
// as rows are used. Every entity of a kind has every component of the kind. Each table
// is a handle issuer (its kind name is the handle kind's name) and a set of resources:
// "kind" for spawn and destroy, "kind.Component" for each column, "*.Component" in a
// system's sets for that column in every kind.
//
//   constinit const KindDesc k_kinds[] = {{.name = ".bullet", .components = "Transform2, Sprite, Velocity", .capacity = 4096}};
//
// A kind's name may start with '.' for "<module>."; the component list is one string.
//
// Rows are zero when spawned unless spawn() is handed the components to write. Iteration
// is by slot in [0, rows()), skipping dead slots, so it is fixed run to run. Destroy is
// immediate here; the phase-boundary deferral and the command path arrive with the
// scheduler.
#pragma once
#include "korvid/core/array.h"
#include "korvid/core/component.h"
#include "korvid/core/pool.h"

namespace kv {

struct KindDesc {
    const char*             name       = nullptr; // also the handle kind and the resource name; '.' prefix: the module's
    const char*             components = nullptr; // component names, comma-separated, resolved at init
    u32                     capacity   = 0;       // slots of address space; pages commit as used
    u32*                    id         = nullptr; // optional: filled with the kind id at init, 0 after shutdown
    bool                    cosmetic   = false;   // presentation only: never part of the state hash (glyphs, an overlay)
};

constexpr u32 KV_KIND_NAME_MAX = 48;

struct Column {
    const ComponentDesc* desc   = nullptr;
    u32                  stride = 0;       // size rounded up to the component's alignment
    u8*                  data   = nullptr; // by slot, capacity * stride reserved
    u8*                  prev   = nullptr; // previous tick, when the component declares prev
    u32                  resource = 0;     // id of "kind.Component"
    char                 resource_name[2 * KV_KIND_NAME_MAX + 2];
};

struct Table {
    const KindDesc* desc     = nullptr;
    const char*     name     = nullptr; // the kind's full name: desc->name with the module prefix expanded
    u32             index    = 0; // position in the engine's table list; kind id = index + 1
    u32             module   = 0; // declaring module's index
    u16             kind     = 0; // handle kind
    u32             resource = 0; // id of "kind"
    Pool<u8>        slots;        // liveness and generations; the payload is unused
    Array<Column>   columns;      // in the kind's declared order
    u32             committed_rows = 0;
    MemTag          tag;

    // Reserves the columns and registers the handle kind. `comps` is in declared order;
    // `full_name` is the kind's name with its module prefix expanded (interned, stable).
    bool init(const KindDesc& d, const char* full_name, Span<const ComponentDesc* const> comps, u32 table_index, u32 module_index, MemTag t);
    void free();

    Handle spawn(); // every component zero, prev included; null if the kind is full
    template <class... C>
    Handle spawn(const C&... c) {
        Handle h = spawn();
        if (!h.is_null()) (set_row(h, c), ...);
        return h;
    }
    bool destroy(Handle h);
    // Every live entity, at once. Returns how many went.
    u32  destroy_all();

    bool   valid(Handle h) const { return slots.valid(h); }
    u32    live() const { return slots.live; }
    u32    rows() const { return slots.capacity(); } // high-water slot count; iterate with is_alive
    bool   is_alive(u32 i) const { return slots.is_alive(i); }
    Handle handle_at(u32 i) const { return slots.handle_at(i); }

    const Column* column(u32 component_id) const;
    Column*       column(u32 component_id) { return (Column*)((const Table*)this)->column(component_id); }
    template <class T> Column*       column()       { return column(ComponentType<T>::id); }
    template <class T> const Column* column() const { return column(ComponentType<T>::id); }
    bool has(u32 component_id) const { return column(component_id) != nullptr; }

    // Typed access. Null for a stale handle or a component the kind lacks.
    template <class T>
    T* get(Handle h) {
        Column* c = column<T>();
        return c && valid(h) ? (T*)(c->data + (usize)h.index() * c->stride) : nullptr;
    }
    template <class T>
    const T* get(Handle h) const { return ((Table*)this)->get<T>(h); }
    template <class T>
    T* prev(Handle h) {
        Column* c = column<T>();
        return c && c->prev && valid(h) ? (T*)(c->prev + (usize)h.index() * c->stride) : nullptr;
    }
    template <class T>
    const T* prev(Handle h) const { return ((Table*)this)->prev<T>(h); }

    // Whole columns over [0, rows()), dead slots included: pair with is_alive(i).
    // Valid only when stride == sizeof(T), which holds for any T whose size is a multiple
    // of its alignment, that is, every C++ struct.
    template <class T>
    Span<T> rows_of() {
        Column* c = column<T>();
        return c ? Span<T>((T*)c->data, rows()) : Span<T>();
    }
    template <class T>
    Span<const T> rows_of() const {
        const Column* c = column<T>();
        return c ? Span<const T>((const T*)c->data, rows()) : Span<const T>();
    }
    template <class T>
    Span<const T> prev_rows_of() const {
        const Column* c = column<T>();
        return c && c->prev ? Span<const T>((const T*)c->prev, rows()) : Span<const T>();
    }

    // A load: every entity goes and the slots become exactly these (Pool::restore_slots):
    // the live ones zeroed, prev included, for the caller to fill. Spawns after it hand out
    // the handles the saved table would have. False, and the table empty, if the slots do
    // not fit the capacity or disagree with themselves.
    bool restore_slots(Span<const u32> gen_words, Span<const u32> free_order);

    // Top of PreSim: the current rows become the previous ones, for every prev column.
    void begin_tick();

    // "Transform2{...} Sprite{...}" for a live entity; the slot's state otherwise.
    int describe(Handle h, char* out, usize cap) const;

    // Every sim column of every live row in slot order, each row with its generation and
    // index. The same world hashes the same in any process or build.
    u64 hash() const;

private:
    template <class T>
    void set_row(Handle h, const T& v) {
        T* p = get<T>(h);
        KV_ASSERT(p); // the kind has no such component
        if (!p) return;
        *p = v;
        if (T* q = prev<T>(h)) *q = v; // a new entity has no history
    }
    bool ensure_rows(u32 n);
    u8*  row(const Column& c, u32 i) const { return c.data + (usize)i * c.stride; }
};

inline u64 table_hash(const Table& t) { return t.hash(); }

} // namespace kv
