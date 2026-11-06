// Korvid core: a growable array of plain data. Explicit free(), no destructor.
#pragma once
#include "korvid/core/memory.h"

namespace kv {

template <class T>
struct Array {
    static_assert(__is_trivially_copyable(T), "Array holds plain data only");

    T*  data = nullptr;
    u32 len  = 0;
    u32 cap  = 0;

    void reserve(u32 n) {
        if (n <= cap) return;
        u32 c = cap ? cap : 8;
        while (c < n) c *= 2;
        data = (T*)mem_realloc(data, (usize)c * sizeof(T));
        cap  = c;
    }

    T& push(const T& v) {
        reserve(len + 1);
        data[len] = v;
        return data[len++];
    }

    // Appends n uninitialized slots and returns a pointer to the first.
    T* push_n(u32 n) {
        reserve(len + n);
        T* p = data + len;
        len += n;
        return p;
    }

    T pop() {
        KV_ASSERT(len > 0);
        return data[--len];
    }

    // Grows with copies of v until len == n. Never shrinks.
    void resize_fill(u32 n, const T& v) {
        if (n <= len) return;
        reserve(n);
        for (u32 i = len; i < n; ++i) data[i] = v;
        len = n;
    }

    void clear() { len = 0; }

    void free() {
        mem_free(data);
        data = nullptr;
        len = cap = 0;
    }

    T&       operator[](u32 i)       { KV_ASSERT(i < len); return data[i]; }
    const T& operator[](u32 i) const { KV_ASSERT(i < len); return data[i]; }
    T&       last()                  { KV_ASSERT(len > 0); return data[len - 1]; }
    T*       begin()       { return data; }
    T*       end()         { return data + len; }
    const T* begin() const { return data; }
    const T* end()   const { return data + len; }
    bool     empty() const { return len == 0; }

    Span<T>       span()       { return Span<T>(data, len); }
    Span<const T> span() const { return Span<const T>(data, len); }
};

} // namespace kv
