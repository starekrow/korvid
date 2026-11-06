// Korvid core: a generational slot pool. Backs entity pools and component storage.
// A stale handle returns null, never another slot's data.
#pragma once
#include "korvid/core/array.h"
#include "korvid/core/handle.h"

namespace kv {

template <class T>
struct Pool {
    Array<T>   items;
    Array<u32> gens;
    Array<u8>  alive;
    Array<u32> free_list;
    u32        live = 0;

    Handle create(const T& v) {
        u32 idx;
        if (!free_list.empty()) {
            idx = free_list.pop();
            items[idx] = v;
        } else {
            idx = items.len;
            items.push(v);
            gens.push(1);
            alive.push(0);
        }
        alive[idx] = 1;
        ++live;
        return Handle{idx, gens[idx]};
    }

    bool valid(Handle h) const {
        return h.gen != 0 && h.index < items.len && alive[h.index] && gens[h.index] == h.gen;
    }

    T*       get(Handle h)       { return valid(h) ? &items[h.index] : nullptr; }
    const T* get(Handle h) const { return valid(h) ? &items[h.index] : nullptr; }

    bool destroy(Handle h) {
        if (!valid(h)) return false;
        alive[h.index] = 0;
        u32 g = gens[h.index] + 1;
        if (g == 0) g = 1;
        gens[h.index] = g;
        free_list.push(h.index);
        --live;
        return true;
    }

    // Slot count including dead slots; iterate with alive[] to skip them.
    u32 capacity() const { return items.len; }

    void free() {
        items.free();
        gens.free();
        alive.free();
        free_list.free();
        live = 0;
    }
};

} // namespace kv
