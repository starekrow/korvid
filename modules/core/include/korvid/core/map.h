// Korvid core: Map, the hash map of plain data.
//
// Dense entries (hashes, keys, values in parallel arrays, insertion order) plus, once
// there are enough of them, an open-addressing index of slots into the dense arrays.
// Below the threshold there is no index at all: a lookup scans the hash array, which
// beats a table for a few entries. Promotion builds the index once; it is never torn down
// on shrink.
//
// Iteration is a linear walk over the dense arrays, keys[i] and values[i] for i below
// len(). remove() swaps the last entry into the hole, so it reorders one entry. Growth
// may move the arrays; pointers from find() are valid until the next insert or remove.
// Deterministic: the same operations give the same order, on any platform, so walking a
// map is safe in sim code.
#pragma once
#include "korvid/core/array.h"
#include "korvid/core/hash.h"
#include <string.h>

namespace kv {

constexpr u32 KV_MAP_SMALL = 16; // entries scanned without an index; 16 hashes = two lines

// A map from K to V, both plain data. A key needs hash_key(k) (hash.h; integers, Name and
// Handle have one) and ==. A zero-initialized Map is empty and ready, on the general
// allocator; init(alloc) puts it somewhere else, before the first insert.
//   Map<u64, u32> by_name;
//   by_name.init(&self.general);
//   by_name.set(name.hash, slot);
//   if (u32* s = by_name.find(name.hash)) use(*s);
//   for (u32 i = 0; i < by_name.len(); ++i) visit(by_name.keys[i], by_name.values[i]);
template <class K, class V>
struct Map {
    static_assert(__is_trivially_copyable(K) && __is_trivially_copyable(V), "Map holds plain data only");

    Array<u64> hashes;
    Array<K>   keys;
    Array<V>   values;
    u32*       index     = nullptr; // slot -> dense index + 1; 0 is empty. Null in small mode.
    u32        index_cap = 0;       // power of two
    Allocator* alloc     = nullptr; // null: general

    // Sets the allocator for every array inside: use this, not `alloc` alone.
    void init(Allocator* a) { alloc = hashes.alloc = keys.alloc = values.alloc = a; }
    u32  len() const { return keys.len; }
    bool empty() const { return keys.len == 0; }

    // The value for k, or null. Valid until the next insert or remove.

    V*       find(const K& k)       { u32 d = find_dense(k, hash_key(k)); return d == NONE ? nullptr : &values.data[d]; }
    const V* find(const K& k) const { u32 d = find_dense(k, hash_key(k)); return d == NONE ? nullptr : &values.data[d]; }
    bool     has(const K& k)  const { return find_dense(k, hash_key(k)) != NONE; }

    // Inserts or overwrites. Returns the stored value.
    V& set(const K& k, const V& v) {
        u64 h = hash_key(k);
        u32 d = find_dense(k, h);
        if (d != NONE) { values.data[d] = v; return values.data[d]; }
        return *insert_new(k, h, v);
    }

    // The value for k, inserting `dflt` if absent. *inserted says which.
    V* get_or_insert(const K& k, const V& dflt, bool* inserted = nullptr) {
        u64 h = hash_key(k);
        u32 d = find_dense(k, h);
        if (inserted) *inserted = d == NONE;
        return d == NONE ? insert_new(k, h, dflt) : &values.data[d];
    }

    // Swap-removes. The last entry moves into the hole.
    bool remove(const K& k) {
        u64 h = hash_key(k);
        u32 d = find_dense(k, h);
        if (d == NONE) return false;
        u32 last = keys.len - 1;
        if (index) {
            index_erase(h, d);
            if (d != last) index_repoint(hashes.data[last], last, d);
        }
        if (d != last) {
            hashes.data[d] = hashes.data[last];
            keys.data[d]   = keys.data[last];
            values.data[d] = values.data[last];
        }
        hashes.len = keys.len = values.len = last;
        return true;
    }

    // Empties the map and keeps its memory.
    void clear() {
        hashes.clear(); keys.clear(); values.clear();
        if (index) memset(index, 0, sizeof(u32) * index_cap);
    }

    // Gives the memory back: call it when the owner is done (a module's shutdown).
    void free() {
        hashes.free(); keys.free(); values.free();
        if (index) { Allocator* a = allocator(); a->free(a, index, sizeof(u32) * index_cap); }
        index = nullptr;
        index_cap = 0;
    }

    // Room for n entries without growing the dense arrays.
    void reserve(u32 n) { hashes.reserve(n); keys.reserve(n); values.reserve(n); }

private:
    static constexpr u32 NONE = 0xFFFFFFFFu;

    Allocator* allocator() const { return alloc ? alloc : mem_general(); }

    u32 find_dense(const K& k, u64 h) const {
        if (!index) { // small mode: scan the hashes, which sit together
            for (u32 i = 0; i < hashes.len; ++i)
                if (hashes.data[i] == h && keys.data[i] == k) return i;
            return NONE;
        }
        u32 mask = index_cap - 1;
        for (u32 s = (u32)h & mask;; s = (s + 1) & mask) {
            u32 e = index[s];
            if (e == 0) return NONE;
            u32 d = e - 1;
            if (hashes.data[d] == h && keys.data[d] == k) return d;
        }
    }

    V* insert_new(const K& k, u64 h, const V& v) {
        u32 d = keys.len;
        hashes.push(h);
        keys.push(k);
        values.push(v);
        if (!index) {
            if (keys.len > KV_MAP_SMALL) rebuild_index(32);
        } else {
            if ((u64)keys.len * 4 > (u64)index_cap * 3) rebuild_index(index_cap * 2);
            else index_insert(h, d);
        }
        return &values.data[d];
    }

    void index_insert(u64 h, u32 d) {
        u32 mask = index_cap - 1;
        for (u32 s = (u32)h & mask;; s = (s + 1) & mask)
            if (index[s] == 0) { index[s] = d + 1; return; }
    }

    // Backward-shift deletion: no tombstones, probe runs stay tight.
    void index_erase(u64 h, u32 d) {
        u32 mask = index_cap - 1;
        u32 s = (u32)h & mask;
        while (index[s] != d + 1) s = (s + 1) & mask;
        for (u32 j = (s + 1) & mask;; j = (j + 1) & mask) {
            u32 e = index[j];
            if (e == 0) break;
            u32 home = (u32)hashes.data[e - 1] & mask;
            // Move e back into s unless its home lies in (s, j] cyclically.
            bool in_range = s <= j ? (home > s && home <= j) : (home > s || home <= j);
            if (!in_range) { index[s] = e; s = j; }
        }
        index[s] = 0;
    }

    // The dense entry `from` moved to `to`; find its slot and repoint it.
    void index_repoint(u64 h, u32 from, u32 to) {
        u32 mask = index_cap - 1;
        for (u32 s = (u32)h & mask;; s = (s + 1) & mask)
            if (index[s] == from + 1) { index[s] = to + 1; return; }
    }

    void rebuild_index(u32 cap) {
        while (cap < keys.len * 2) cap *= 2; // never above half full after a rebuild
        Allocator* a = allocator();
        if (index) a->free(a, index, sizeof(u32) * index_cap);
        index     = (u32*)a->alloc(a, sizeof(u32) * cap, KV_MEM_DEFAULT_ALIGN);
        index_cap = cap;
        memset(index, 0, sizeof(u32) * cap);
        for (u32 d = 0; d < keys.len; ++d) index_insert(hashes.data[d], d);
    }
};

} // namespace kv
