// Korvid core: a generational slot pool. Backs entity pools and component storage.
// A stale handle returns null, never another slot's data; a handle from another pool
// fails the kind check for the same cost.
//
// The pool is handle-compatible, not a handle issuer: it stamps whatever kind its owner
// hands it (use_kind) and otherwise issues local handles with kind 0, which the registry
// treats as nothing. The owner registers the kind (kv_handle.h) and decides what a
// resolved pointer means; resolve_fn and describe_fn are offered for owners whose answer
// is "the item". A pool is plain data and may be moved by value; an owner that
// registered it as ctx updates the ctx. Freed slots are reused FIFO, so generations
// wrap only after the whole pool has cycled.
//
// Storage is one reserved range committed as slots are used, so slot addresses are
// stable for the pool's lifetime and the whole pool is relocatable: a snapshot is a copy
// of the used prefix of each section. Sections, each page aligned: items, generation
// words (generation in the low 20 bits, alive in bit 31, so validation reads one line),
// free-list links. Single owner.
#pragma once
#include "korvid/core/handle.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/os_memory.h"
#include "korvid/core/thread.h"
#include <string.h>

namespace kv {

constexpr u32 KV_POOL_DEFAULT_MAX = 1u << 20; // slots of address space, not memory
constexpr u32 KV_POOL_NONE        = 0xFFFFFFFFu;

struct PoolSnapshotHeader {
    u32 len, live, free_head, free_tail, item_size;
};

template <class T>
struct Pool {
    static_assert(__is_trivially_copyable(T), "Pool holds plain data only");

    static constexpr u32 ALIVE_BIT = 0x80000000u;

    T*    items     = nullptr; // by slot
    u32*  gens      = nullptr; // by slot: generation | ALIVE_BIT; generations start at 1, skip 0 on wrap
    u32*  next_free = nullptr; // by slot: intrusive free list
    u32   len       = 0;       // slots in use, dead or alive
    u32   live      = 0;
    u32   free_head = KV_POOL_NONE; // FIFO: pop here
    u32   free_tail = KV_POOL_NONE; //       push here
    u16   kind      = 0;            // stamped into issued handles; 0 means local
    u32   max_items = 0;
    u32   committed = 0;       // slots whose pages are committed
    u8*   base      = nullptr;
    usize reserved  = 0;
    usize off_gens = 0, off_next = 0;
    MemTag tag;
    u32    owner = 0;

    // Reserves address space for up to max slots. Called on first create if not before.
    bool init(u32 max = KV_POOL_DEFAULT_MAX, MemTag t = MemTag{}) {
        KV_ASSERT(!base);
        max_items = max;
        tag       = t;
        owner     = thread_id();
        usize items_bytes = os_page_align((usize)max * sizeof(T));
        usize gens_bytes  = os_page_align((usize)max * sizeof(u32));
        usize next_bytes  = os_page_align((usize)max * sizeof(u32));
        off_gens  = items_bytes;
        off_next  = off_gens + gens_bytes;
        reserved  = off_next + next_bytes;
        base = (u8*)os_reserve(reserved);
        if (!base) {
            KV_ERROR("mem", "pool: cannot reserve %llu bytes", (unsigned long long)reserved);
            return false;
        }
        items     = (T*)base;
        gens      = (u32*)(base + off_gens);
        next_free = (u32*)(base + off_next);
        return true;
    }

    Handle create(const T& v) {
        if (!base && !init()) return Handle{};
        KV_VERIFY(owner == thread_id());
        u32 idx;
        if (free_head != KV_POOL_NONE) {
            idx       = free_head;
            free_head = next_free[idx];
            if (free_head == KV_POOL_NONE) free_tail = KV_POOL_NONE;
        } else {
            if (len >= max_items) { KV_ERROR("mem", "pool is full: %u slots", max_items); return Handle{}; }
            if (len >= committed && !commit_more()) return Handle{};
            idx = len++;
            gens[idx] = 1;
        }
        items[idx] = v;
        gens[idx] |= ALIVE_BIT;
        next_free[idx] = KV_POOL_NONE;
        ++live;
        return handle_at(idx);
    }

    // The owner registered a kind; from now on issued handles carry it and valid()
    // rejects handles that do not. Set before the first create.
    void use_kind(u16 k) { KV_ASSERT(len == 0); kind = k; }

    // For an owner that registers the pool itself as ctx: resolve is the item, describe
    // is the slot's state. An owner with its own meaning writes its own.
    static void* resolve_fn(kv_handle h, void* ctx) { return ((Pool*)ctx)->get(Handle{h}); }
    static int   describe_fn(kv_handle h, void* ctx, char* out, size_t cap) {
        const Pool& p = *(const Pool*)ctx;
        Handle hh{h};
        const char* state = p.valid(hh) ? "alive" : hh.index() < p.len ? "stale" : "out of range";
        return format(out, cap, "%s (%u live of %u slots)", state, p.live, p.len);
    }

    // The handle for a slot, whatever its state. For iteration with is_alive().
    Handle handle_at(u32 i) const { return Handle::make(kind, gens[i] & ~ALIVE_BIT, i); }
    bool   is_alive(u32 i) const { return i < len && (gens[i] & ALIVE_BIT); }

    bool valid(Handle h) const {
        u32 i = h.index();
        return h.kind() == kind && h.gen() != 0 && i < len && gens[i] == (h.gen() | ALIVE_BIT);
    }

    T*       get(Handle h)       { return valid(h) ? &items[h.index()] : nullptr; }
    const T* get(Handle h) const { return valid(h) ? &items[h.index()] : nullptr; }

    bool destroy(Handle h) {
        if (!valid(h)) return false;
        KV_VERIFY(owner == thread_id());
        u32 i = h.index();
#if KV_DEV
        if (!dev_spike_hunting()) memset((void*)&items[i], 0xDD, sizeof(T)); // a stale pointer sees poison, not old data
#endif
        u32 g = (gens[i] & ~ALIVE_BIT) + 1;
        if (g > KV_HANDLE_MAX_GEN) g = 1; // skip 0 on wrap
        gens[i] = g; // dead: no alive bit
        // Append: the slot comes back only after every other free slot has been reused.
        next_free[i] = KV_POOL_NONE;
        if (free_tail == KV_POOL_NONE) free_head = i;
        else next_free[free_tail] = i;
        free_tail = i;
        --live;
        return true;
    }

    // A load: the pool becomes gen_words.len slots with these words (the generation, with
    // ALIVE_BIT for a live slot), every item zero, the dead slots free in the order
    // `free_order` gives and any it leaves out after them in slot order. A pool so restored
    // hands out the same handles, in the same order, as the one that was saved. False, and
    // the pool empty, for more slots than max, a generation of 0 or past the limit, or a
    // free list naming a live, repeated or missing slot.
    bool restore_slots(Span<const u32> gen_words, Span<const u32> free_order) {
        if (!base && !init()) return false;
        u32 n = gen_words.len;
        auto fail = [&](const char* why) {
            KV_ERROR("mem", "pool restore: %s", why);
            len = live = 0;
            free_head = free_tail = KV_POOL_NONE;
            return false;
        };
        if (n > max_items) return fail("more slots than the pool has");
        while (committed < n) if (!commit_more()) return fail("commit failed");
        constexpr u32 kDeadUnlisted = KV_POOL_NONE - 1;
        live = 0;
        for (u32 i = 0; i < n; ++i) {
            u32 g = gen_words[i] & ~ALIVE_BIT;
            if (g == 0 || g > KV_HANDLE_MAX_GEN) return fail("a generation out of range");
            gens[i] = gen_words[i];
            if (gens[i] & ALIVE_BIT) ++live;
            next_free[i] = gens[i] & ALIVE_BIT ? KV_POOL_NONE : kDeadUnlisted;
        }
        len = n;
        memset((void*)items, 0, (usize)n * sizeof(T));
        free_head = free_tail = KV_POOL_NONE;
        auto append = [&](u32 i) {
            next_free[i] = KV_POOL_NONE;
            if (free_tail == KV_POOL_NONE) free_head = i;
            else next_free[free_tail] = i;
            free_tail = i;
        };
        for (u32 i : free_order) {
            if (i >= n || next_free[i] != kDeadUnlisted) return fail("the free list names a live, repeated or missing slot");
            append(i);
        }
        for (u32 i = 0; i < n; ++i)
            if (next_free[i] == kDeadUnlisted) append(i);
        return true;
    }

    // The dead slots in the order create() will reuse them.
    template <class F>
    void each_free(F&& fn) const {
        for (u32 i = free_head; i != KV_POOL_NONE; i = next_free[i]) fn(i);
    }

    // Slot count including dead slots; iterate with is_alive() to skip them.
    u32 capacity() const { return len; }

    void free() {
        if (base) {
            mem_account_decommit(tag, committed_bytes());
            os_release(base, reserved);
        }
        *this = Pool{};
    }

    // ---- snapshot: the used prefix of every section, relocatable ----
    usize snapshot_size() const {
        return sizeof(PoolSnapshotHeader) + (usize)len * (sizeof(T) + sizeof(u32) + sizeof(u32));
    }
    // Writes snapshot_size() bytes; the caller sized `out`.
    void snapshot(void* out) const {
        u8* p = (u8*)out;
        PoolSnapshotHeader h{len, live, free_head, free_tail, (u32)sizeof(T)};
        memcpy(p, &h, sizeof h);              p += sizeof h;
        memcpy(p, items, (usize)len * sizeof(T));      p += (usize)len * sizeof(T);
        memcpy(p, gens, (usize)len * sizeof(u32));     p += (usize)len * sizeof(u32);
        memcpy(p, next_free, (usize)len * sizeof(u32));
    }
    // Restores into this pool (initialized or not). Slot addresses stay where they are,
    // and so does this pool's kind: handles issued by another pool stay foreign.
    bool restore(const void* in) {
        if (!base && !init()) return false;
        const u8* p = (const u8*)in;
        PoolSnapshotHeader h;
        memcpy(&h, p, sizeof h); p += sizeof h;
        if (h.item_size != sizeof(T) || h.len > max_items) return false;
        while (committed < h.len) if (!commit_more()) return false;
        memcpy(items, p, (usize)h.len * sizeof(T));    p += (usize)h.len * sizeof(T);
        memcpy(gens, p, (usize)h.len * sizeof(u32));   p += (usize)h.len * sizeof(u32);
        memcpy(next_free, p, (usize)h.len * sizeof(u32));
        len = h.len; live = h.live; free_head = h.free_head; free_tail = h.free_tail;
        return true;
    }

private:
    usize committed_bytes() const {
        return os_page_align((usize)committed * sizeof(T)) + os_page_align((usize)committed * sizeof(u32)) * 2;
    }
    // Commits the next run of slots in every section.
    bool commit_more() {
        u32 grow = committed ? committed : 1024;
        if (committed + grow > max_items) grow = max_items - committed;
        if (grow == 0) return false;
        usize before = committed_bytes();
        u32 to = committed + grow;
        usize after = os_page_align((usize)to * sizeof(T)) + os_page_align((usize)to * sizeof(u32)) * 2;
        if (!mem_heap_admit(tag, after - before)) return false; // the hard limit said so, with the message
        bool ok = commit_section(0,        sizeof(T), to) &&
                  commit_section(off_gens, sizeof(u32), to) &&
                  commit_section(off_next, sizeof(u32), to);
        if (!ok) { KV_ERROR("mem", "pool: commit failed"); return false; }
        committed = to;
        mem_account_commit(tag, committed_bytes() - before);
        return true;
    }
    bool commit_section(usize off, usize elem, u32 to) {
        usize have = os_page_align((usize)committed * elem);
        usize need = os_page_align((usize)to * elem);
        return need <= have || os_commit(base + off + have, need - have);
    }
};

} // namespace kv
