// Korvid core: ktables, the engine's entity storage.
//
// An entity is a row in a ktable, and its handle says which. A ktable is a named, fixed
// set of components with a capacity, declared by a module as data (a KTableDesc);
// engine_init makes each one a KTable. Every entity of a ktable has every component of the
// ktable, and there is no adding or removing one at runtime. That's what lets a system
// walk a ktable as columns (one array per component, indexed by slot), and what lets any
// module act on any ktable that has its components: a ktable with Transform2 and Sprite
// is drawn, one with Transform2 and Collider2 collides, whoever declared it.
//
//   constinit u32 g_rock_ktable = 0;
//   constinit const KTableDesc k_ktables[] = {
//       {.name = ".rock", .components = "Transform2, Sprite, Velocity, Collider2", .capacity = 1024, .id = &g_rock_ktable},
//   };
//
// Component names resolve across modules (Transform2 is the sprite module's). Capacity
// is address space: pages commit as rows are used, so a generous capacity costs nothing
// until it fills. A full ktable refuses the spawn with a null handle, logged once.
//
// In a system's sets, a ktable is a tree of resources: ".rock" is all of it (a write on it
// covers spawning, destroying and every column), ".rock.@alive" which slots are alive,
// ".rock.Velocity" one column, and "*.Velocity" that column in every ktable that has it.
// Systems reach a ktable through the context (ctx.view, ctx.write_ktable); the KTable
// methods below act at once, and are for code that holds the ktable's write or runs
// outside the schedule. Iteration is by slot in [0, rows()), dead slots included and
// skipped with is_alive, so the order is the same every run.
#pragma once
#include "korvid/core/array.h"
#include "korvid/core/component.h"
#include "korvid/core/name.h"
#include "korvid/core/pool.h"

namespace kv {

// How a system's early read ('^') of a ktable gets the phase's starting value. Order runs
// the reader before every writer of what it reads early: nearly free, but it adds edges.
// Snapshot copies what is read early and written at the phase start and hands the reader
// the copy, with no order at all: the reader can run beside the writers, or a cycle the
// edges would make is not there. The reader sees the same values either way.
enum class EarlyReads : u8 { Order, Snapshot };

// One of a module's ktable declarations (example at the top). name, components and
// capacity are required.
struct KTableDesc {
    const char*             name       = nullptr; // ".rock": the module's own; also the handle kind and the resource name
    const char*             components = nullptr; // "Transform2, Sprite, Velocity": component names, from any module
    u32                     capacity   = 0;       // most entities alive at once; address space, committed as used
    u32*                    id         = nullptr; // optional: filled with the ktable id at init, 0 after shutdown
    bool                    cosmetic   = false;   // presentation only: never part of the state hash (glyphs, an overlay)
    // Rows that never change once spawned (a map's tiles): the engine copies their previous
    // values and the sprite extract looks at them only when the ktable has spawned or lost
    // a row since it last did. A change to a living row of a still ktable is not seen.
    bool                    still      = false;
    // Per entity, not per ktable: components an entity of this ktable may carry one of on
    // its own ("Talk, Quest"), stored sparsely as resources "ktable.@Talk"; and a property
    // bag of named values per entity ("ktable.@props"). For what one villager or one chest
    // has and the rest don't. Engine systems never see either, since views are built from
    // columns: an attached Sprite draws nothing. See ctx.attach and ctx.prop_set_int.
    const char*             attachable = nullptr;
    bool                    props      = false;
    EarlyReads              early_reads = EarlyReads::Order; // how early reads of it get the phase start (see EarlyReads)
};

constexpr u32 KV_KTABLE_NAME_MAX = 48; // bytes of a ktable's full name, NUL included

// ---- internal: how a ktable stores its rows ----------------------------------------
// One component's storage in a ktable: a column of rows by slot.
struct Column {
    const ComponentDesc* desc   = nullptr;
    u32                  stride = 0;       // size rounded up to the component's alignment
    u8*                  data   = nullptr; // by slot, capacity * stride reserved
    u8*                  prev   = nullptr; // previous tick, when the component declares prev
    u32                  resource = 0;     // id of "ktable.Component"
    char                 resource_name[2 * KV_KTABLE_NAME_MAX + 2];
};

// What some entities of a ktable have and the rest do not: their names, their attachments,
// their property bags. Entries by slot, ascending, each with the generation of the entity
// it was made for, so an entry can outlive its entity only as a stale one: readers skip it
// (KTable::sparse_live) and the engine sweeps it at the next phase boundary. Lookups are a
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

// An attachable component's store in one ktable: its schema, its entries by slot, and the
// resource "ktable.@Component" systems name to reach it.
struct Attachment {
    const ComponentDesc* desc     = nullptr;
    SparseRows           rows;
    u32                  resource = 0;
    char                 resource_name[2 * KV_KTABLE_NAME_MAX + 3];
};

// ---- property bag values -------------------------------------------------------------
// A property bag's values: named, typed, few. Strings are interned names; handles are
// written by name in saves like any other. Systems use the context's typed forms
// (ctx.prop_int, ctx.prop_set_str); these are the raw values KTable::prop hands out.
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

// ---- the ktable -----------------------------------------------------------------------
// A ktable as engine_init makes it: liveness and generations (a Pool of slots), one column
// per component, and the sparse rows for names, attachments and bags. Each ktable is also
// a handle issuer: its handles print as "rocks.rock#12:3" and resolve to it.
//
// Its methods act at once. Spawning and destroying here is what ctx.write_ktable gives a
// system that holds the ktable's write; everyone else uses the commands (ctx.spawn_later,
// ctx.destroy), which wait for the phase boundary.
//   KTable* rocks = ctx.write_ktable(g_rock_ktable);
//   Handle h = rocks->spawn(transform2_at(at), Velocity{v, 0.0f});   // the rest zero
//   if (Velocity* vel = rocks->get<Velocity>(h)) vel->v *= 0.5f;
struct KTable {
    const KTableDesc* desc   = nullptr;
    const char*     name     = nullptr; // the ktable's full name: desc->name with the module prefix expanded
    u32             index    = 0; // position in the engine's ktable list; ktable id = index + 1
    u32             module   = 0; // declaring module's index
    u16             handle_kind = 0; // the handle kind its entities carry
    u32             resource = 0; // id of "ktable": all of it
    u32             alive_resource = 0; // id of "ktable.@alive": which slots are alive
    Pool<u8>        slots;        // liveness and generations; the payload is unused
    Array<Column>   columns;      // in the ktable's declared order
    u32             committed_rows = 0;
    MemTag          tag;
    SparseRows      names;        // entity names (Name), by slot; the engine keeps the reverse map
    Array<Attachment> attachments; // one per attachable component, in the ktable's declared order
    Array<PropEntry> props;       // by slot, then key hash; the bag of every entity that has one
    u32             props_resource = 0; // "ktable.@props", when the ktable has bags
    char            props_resource_name[2 * KV_KTABLE_NAME_MAX + 3] = {};
    bool            sparse_dirty = false; // an entity with sparse rows died: the engine sweeps at the boundary
    u32             capacity = 0;  // slots: the KTableDesc's, unless the root overrode it (EngineConfig::capacities)
    u64             churn    = 0;  // +1 at every spawn, destroy and restore: what a still ktable's readers watch
    u64             churn_copied = ~0ull; // the churn at the last previous-copy (begin_tick), for a still ktable
    u32             restores = 0;  // +1 at every restore_slots: a restored row may reuse a handle with new values
    u32             refused  = 0;  // spawns refused because the ktable was full; the first is logged

    // Reserves the columns and registers the handle kind: engine_init's. `comps` is in
    // declared order; `full_name` is the ktable's name with its module prefix expanded.
    bool init(const KTableDesc& d, const char* full_name, Span<const ComponentDesc* const> comps, u32 ktable_index, u32 module_index, MemTag t,
              Span<const ComponentDesc* const> attachable = {}, u32 capacity_override = 0);
    void free();

    // A new entity, every component zero (previous copies included) except the ones
    // given, which are written to both: a new entity has no history to interpolate from.
    // Null if the ktable is full. Components may be given in any order; one the ktable
    // lacks asserts in debug builds.
    Handle spawn();
    template <class... C>
    Handle spawn(const C&... c) {
        Handle h = spawn();
        if (!h.is_null()) (set_row(h, c), ...);
        return h;
    }
    // Destroys an entity now: its slot's generation moves on, so its handle and every copy
    // of it stop resolving. False if it was already dead. Dev builds poison its rows.
    bool destroy(Handle h);
    // Every live entity, at once. Returns how many went.
    u32  destroy_all();

    // Whether the handle names a living entity of this ktable.
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
    // ktable cannot attach); false when there was none to detach.
    Attachment*       attachment(u32 component_id);
    const Attachment* attachment(u32 component_id) const { return ((KTable*)this)->attachment(component_id); }
    void*  attached(Handle h, u32 component_id);
    const void* attached(Handle h, u32 component_id) const { return ((KTable*)this)->attached(h, component_id); }
    void*  attach(Handle h, u32 component_id, const void* data);
    bool   detach(Handle h, u32 component_id);
    template <class T> T*   attached(Handle h) { return (T*)attached(h, ComponentType<T>::id); }
    template <class T> T*   attach(Handle h, const T& v) { return (T*)attach(h, ComponentType<T>::id, &v); }

    // The property bag, directly. A value, or null; set (false for a stale handle or a
    // ktable without bags); remove; the entity's entries as a run of `props` (count 0 for none).
    const PropValue* prop(Handle h, Name key) const;
    bool   prop_set(Handle h, Name key, const PropValue& v);
    bool   prop_remove(Handle h, Name key);
    u32    props_of(Handle h, u32* first) const;
    // Rows and slots: live() entities are alive, somewhere in [0, rows()). rows() is the
    // high-water slot count, so a loop over it checks is_alive(i); handle_at(i) is slot
    // i's current entity, and a live handle's index() is its slot.
    u32    live() const { return slots.live; }
    u32    rows() const { return slots.capacity(); }
    bool   is_alive(u32 i) const { return slots.is_alive(i); }
    Handle handle_at(u32 i) const { return slots.handle_at(i); }

    // A component's column, or null if the ktable lacks it; has() says which.
    const Column* column(u32 component_id) const;
    Column*       column(u32 component_id) { return (Column*)((const KTable*)this)->column(component_id); }
    template <class T> Column*       column()       { return column(ComponentType<T>::id); }
    template <class T> const Column* column() const { return column(ComponentType<T>::id); }
    bool has(u32 component_id) const { return column(component_id) != nullptr; }

    // One entity's component, and its previous-tick copy (components with CompPrev only).
    // Null for a stale handle or a component the ktable lacks. Writing the previous copy
    // marks a teleport: the renderer won't interpolate across it.
    template <class T>
    T* get(Handle h) {
        Column* c = column<T>();
        return c && valid(h) ? (T*)(c->data + (usize)h.index() * c->stride) : nullptr;
    }
    template <class T>
    const T* get(Handle h) const { return ((KTable*)this)->get<T>(h); }
    template <class T>
    T* prev(Handle h) {
        Column* c = column<T>();
        return c && c->prev && valid(h) ? (T*)(c->prev + (usize)h.index() * c->stride) : nullptr;
    }
    template <class T>
    const T* prev(Handle h) const { return ((KTable*)this)->prev<T>(h); }

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

    // The save module's load: every entity goes and the slots become exactly these
    // (Pool::restore_slots):
    // the live ones zeroed, prev included, for the caller to fill. Spawns after it hand out
    // the handles the saved ktable would have. False, and the ktable empty, if the slots do
    // not fit the capacity or disagree with themselves.
    bool restore_slots(Span<const u32> gen_words, Span<const u32> free_order);

    // Top of PreSim: the current rows become the previous ones, for every prev column, on
    // the job pool when the ktable is large. The
    // engine's.
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
        KV_ASSERT(p); // the ktable has no such component
        if (!p) return;
        *p = v;
        if (T* q = prev<T>(h)) *q = v; // a new entity has no history
    }
    bool ensure_rows(u32 n);
    u8*  row(const Column& c, u32 i) const { return c.data + (usize)i * c.stride; }
};

inline u64 ktable_hash(const KTable& t) { return t.hash(); }

} // namespace kv
