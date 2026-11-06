// Korvid core: a generational slot pool. Backs entity pools and component storage.
// A stale handle returns null, never another slot's data.
//
// Storage is one reserved range committed as slots are used, so slot addresses are
// stable for the pool's lifetime and the whole pool is relocatable: a snapshot is a copy
// of the used prefix of each section. Sections, each page aligned: items, generations,
// alive flags, free-list links. Single owner.
#pragma once
#include "korvid/core/handle.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/os_memory.h"
#include "korvid/core/thread.h"
#include <string.h>

namespace kv {

constexpr u32 KV_POOL_DEFAULT_MAX = 1u << 20; // slots of address space, not memory
constexpr u32 KV_POOL_NONE        = 0xFFFFFFFFu;

struct PoolSnapshotHeader {
    u32 len, live, free_head, item_size;
};

template <class T>
struct Pool {
    static_assert(__is_trivially_copyable(T), "Pool holds plain data only");

    T*    items     = nullptr; // by slot
    u32*  gens      = nullptr; // by slot; live generations start at 1 and skip 0 on wrap
    u8*   alive     = nullptr; // by slot
    u32*  next_free = nullptr; // by slot: intrusive free list
    u32   len       = 0;       // slots in use, dead or alive
    u32   live      = 0;
    u32   free_head = KV_POOL_NONE;
    u32   max_items = 0;
    u32   committed = 0;       // slots whose pages are committed
    u8*   base      = nullptr;
    usize reserved  = 0;
    usize off_gens = 0, off_alive = 0, off_next = 0;
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
        usize alive_bytes = os_page_align((usize)max);
        usize next_bytes  = os_page_align((usize)max * sizeof(u32));
        off_gens  = items_bytes;
        off_alive = off_gens + gens_bytes;
        off_next  = off_alive + alive_bytes;
        reserved  = off_next + next_bytes;
        base = (u8*)os_reserve(reserved);
        if (!base) {
            KV_ERROR("mem", "pool: cannot reserve %llu bytes", (unsigned long long)reserved);
            return false;
        }
        items     = (T*)base;
        gens      = (u32*)(base + off_gens);
        alive     = base + off_alive;
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
        } else {
            if (len >= max_items) { KV_ERROR("mem", "pool is full: %u slots", max_items); return Handle{}; }
            if (len >= committed && !commit_more()) return Handle{};
            idx = len++;
            gens[idx] = 1;
        }
        items[idx] = v;
        alive[idx] = 1;
        next_free[idx] = KV_POOL_NONE;
        ++live;
        return Handle{idx, gens[idx]};
    }

    bool valid(Handle h) const {
        return h.gen != 0 && h.index < len && alive[h.index] && gens[h.index] == h.gen;
    }

    T*       get(Handle h)       { return valid(h) ? &items[h.index] : nullptr; }
    const T* get(Handle h) const { return valid(h) ? &items[h.index] : nullptr; }

    bool destroy(Handle h) {
        if (!valid(h)) return false;
        KV_VERIFY(owner == thread_id());
        alive[h.index] = 0;
#if KV_DEV
        memset(&items[h.index], 0xDD, sizeof(T)); // a stale pointer sees poison, not old data
#endif
        u32 g = gens[h.index] + 1;
        if (g == 0) g = 1;
        gens[h.index]      = g;
        next_free[h.index] = free_head;
        free_head          = h.index;
        --live;
        return true;
    }

    // Slot count including dead slots; iterate with alive[] to skip them.
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
        return sizeof(PoolSnapshotHeader) + (usize)len * (sizeof(T) + sizeof(u32) + 1 + sizeof(u32));
    }
    // Writes snapshot_size() bytes; the caller sized `out`.
    void snapshot(void* out) const {
        u8* p = (u8*)out;
        PoolSnapshotHeader h{len, live, free_head, (u32)sizeof(T)};
        memcpy(p, &h, sizeof h);              p += sizeof h;
        memcpy(p, items, (usize)len * sizeof(T));      p += (usize)len * sizeof(T);
        memcpy(p, gens, (usize)len * sizeof(u32));     p += (usize)len * sizeof(u32);
        memcpy(p, alive, len);                         p += len;
        memcpy(p, next_free, (usize)len * sizeof(u32));
    }
    // Restores into this pool (initialized or not). Slot addresses stay where they are.
    bool restore(const void* in) {
        if (!base && !init()) return false;
        const u8* p = (const u8*)in;
        PoolSnapshotHeader h;
        memcpy(&h, p, sizeof h); p += sizeof h;
        if (h.item_size != sizeof(T) || h.len > max_items) return false;
        while (committed < h.len) if (!commit_more()) return false;
        memcpy(items, p, (usize)h.len * sizeof(T));    p += (usize)h.len * sizeof(T);
        memcpy(gens, p, (usize)h.len * sizeof(u32));   p += (usize)h.len * sizeof(u32);
        memcpy(alive, p, h.len);                       p += h.len;
        memcpy(next_free, p, (usize)h.len * sizeof(u32));
        len = h.len; live = h.live; free_head = h.free_head;
        return true;
    }

private:
    usize committed_bytes() const {
        return os_page_align((usize)committed * sizeof(T)) + os_page_align((usize)committed * sizeof(u32)) * 2 +
               os_page_align((usize)committed);
    }
    // Commits the next run of slots in every section.
    bool commit_more() {
        u32 grow = committed ? committed : 1024;
        if (committed + grow > max_items) grow = max_items - committed;
        if (grow == 0) return false;
        usize before = committed_bytes();
        u32 to = committed + grow;
        bool ok = commit_section(0,         sizeof(T), to) &&
                  commit_section(off_gens,  sizeof(u32), to) &&
                  commit_section(off_alive, 1, to) &&
                  commit_section(off_next,  sizeof(u32), to);
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
