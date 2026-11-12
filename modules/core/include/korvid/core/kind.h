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
#include "korvid/core/name.h"
#include "korvid/core/pool.h"

namespace kv {

// How a system's early read ('^') of a kind gets the phase's starting value. Order runs
// the reader before every writer of what it reads early: nearly free, but it adds edges.
// Snapshot copies what is read early and written at the phase start and hands the reader
// the copy, with no order at all: the reader can run beside the writers, or a cycle the
// edges would make is not there. The reader sees the same values either way.
enum class EarlyReads : u8 { Order, Snapshot };

struct KindDesc {
    const char*             name       = nullptr; // also the handle kind and the resource name; '.' prefix: the module's
    const char*             components = nullptr; // component names, comma-separated, resolved at init
    u32                     capacity   = 0;       // slots of address space; pages commit as used
    u32*                    id         = nullptr; // optional: filled with the kind id at init, 0 after shutdown
    bool                    cosmetic   = false;   // presentation only: never part of the state hash (glyphs, an overlay)
    // Rows that never change once spawned (a map's tiles): the engine copies their previous
    // values and the sprite extract looks at them only when the kind has spawned or lost a
    // row since it last did. A change to a living row of a still kind is not seen.
    bool                    still      = false;
    // Per entity, not per kind. Components an entity of this kind may carry one of on its
    // own ("Talk, Quest"), stored sparsely as resources "kind.@Talk"; and a property bag of
    // named values per entity ("kind.@props"). Engine systems never see either: views are
    // built from columns. See ctx.attach and ctx.prop_set.
    const char*             attachable = nullptr;
    bool                    props      = false;
    EarlyReads              early_reads = EarlyReads::Order; // how early reads of it get the phase start (see EarlyReads)
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

// What some entities of a kind have and the rest do not: their names, their attachments,
// their property bags. Entries by slot, ascending, each with the generation of the entity
// it was made for, so an entry can outlive its entity only as a stale one: readers skip it
// (Table::sparse_live) and the engine sweeps it at the next phase boundary. Lookups are a
// binary search; inserts and erases move the tail, which is fine for data that only some
// entities carry and that changes a few times a tick.
struct SparseRows {
    Array<u32> slots;  // ascending
    Array<u32> gens;
    Array<u8>  data;   // stride bytes per entry
    u32        stride = 0;

    u32       count() const { return slots.len; }
    u32       find(u32 slot) const;                // the entry's index, or KV_POOL_NONE
    u8*       at(u32 i) { return data.data + (usize)i * stride; }
    const u8* at(u32 i) const { return data.data + (usize)i * stride; }
    // The entry for `slot`, zeroed and stamped with `gen`: made, or reset if one is there.
    u8*       insert(u32 slot, u32 gen);
    void      erase(u32 i);
    void      clear() { slots.clear(); gens.clear(); data.clear(); }
    void      free() { slots.free(); gens.free(); data.free(); }
};

// An attachable component's store in one kind: its schema, its entries by slot, and the
// resource "kind.@Component" systems name to reach it.
struct Attachment {
    const ComponentDesc* desc     = nullptr;
    SparseRows           rows;
    u32                  resource = 0;
    char                 resource_name[2 * KV_KIND_NAME_MAX + 3];
};

// A property bag's values: named, typed, few. Strings are interned names; handles are
// written by name in saves like any other.
enum class PropType : u8 { None, Int, Float, Bool, String, Handle };
const char* prop_type_name(PropType t);
struct PropValue {
    PropType type = PropType::None;
    i64      i    = 0;  // Int, Bool
    f64      f    = 0;  // Float
    u64      bits = 0;  // String (the Name's hash), Handle
};
struct PropEntry {
    u32       slot, gen;
    Name      key;
    PropValue value;
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
    SparseRows      names;        // entity names (Name), by slot; the engine keeps the reverse map
    Array<Attachment> attachments; // one per attachable component, in the kind's declared order
    Array<PropEntry> props;       // by slot, then key hash; the bag of every entity that has one
    u32             props_resource = 0; // "kind.@props", when the kind has bags
    char            props_resource_name[2 * KV_KIND_NAME_MAX + 3] = {};
    bool            sparse_dirty = false; // an entity with sparse rows died: the engine sweeps at the boundary
    u32             capacity = 0;  // slots: the KindDesc's, unless the root overrode it (EngineConfig::capacities)
    u64             churn    = 0;  // +1 at every spawn, destroy and restore: what a still kind's readers watch
    u64             churn_copied = ~0ull; // the churn at the last previous-copy (begin_tick), for a still kind
    u32             restores = 0;  // +1 at every restore_slots: a restored row may reuse a handle with new values
    u32             refused  = 0;  // spawns refused because the kind was full; the first is logged

    // Reserves the columns and registers the handle kind. `comps` is in declared order;
    // `full_name` is the kind's name with its module prefix expanded (interned, stable).
    bool init(const KindDesc& d, const char* full_name, Span<const ComponentDesc* const> comps, u32 table_index, u32 module_index, MemTag t,
              Span<const ComponentDesc* const> attachable = {}, u32 capacity_override = 0);
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
    // Whether entry i of `s` belongs to the entity living in its slot now.
    bool   sparse_live(const SparseRows& s, u32 i) const { return is_alive(s.slots[i]) && handle_at(s.slots[i]).gen() == s.gens[i]; }
    // The entity's name; null for none or a stale handle.
    Name   name_of(Handle h) const;
    // Drops every entry of the sparse rows whose entity has died. Returns the names dropped
    // into `gone` (for the engine's reverse map), if given.
    void   sweep_sparse(Array<Name>* gone);

    // Attachments, directly (the context's forms check the declared access). The store for
    // a component, or null when it is not attachable here; an entity's entry, or null; a
    // new or replaced entry, copied from `data` (null for a stale handle or a component the
    // kind cannot attach); false when there was none to detach.
    Attachment*       attachment(u32 component_id);
    const Attachment* attachment(u32 component_id) const { return ((Table*)this)->attachment(component_id); }
    void*  attached(Handle h, u32 component_id);
    const void* attached(Handle h, u32 component_id) const { return ((Table*)this)->attached(h, component_id); }
    void*  attach(Handle h, u32 component_id, const void* data);
    bool   detach(Handle h, u32 component_id);
    template <class T> T*   attached(Handle h) { return (T*)attached(h, ComponentType<T>::id); }
    template <class T> T*   attach(Handle h, const T& v) { return (T*)attach(h, ComponentType<T>::id, &v); }

    // The property bag, directly. A value, or null; set (false for a stale handle or a kind
    // without bags); remove; the entity's entries as a run of `props` (count 0 for none).
    const PropValue* prop(Handle h, Name key) const;
    bool   prop_set(Handle h, Name key, const PropValue& v);
    bool   prop_remove(Handle h, Name key);
    u32    props_of(Handle h, u32* first) const;
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
