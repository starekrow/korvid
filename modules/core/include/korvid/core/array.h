// Korvid core: a growable array of plain data. Explicit free(), no destructor.
// Allocates through an Allocator*; null means the general allocator. array_in(alloc)
// makes one that lives in an arena or a scratch, where free() costs nothing.
#pragma once
#include "korvid/core/memory.h"

namespace kv {

template <class T>
struct Array {
    static_assert(__is_trivially_copyable(T), "Array holds plain data only");

    T*         data  = nullptr;
    u32        len   = 0;
    u32        cap   = 0;
    Allocator* alloc = nullptr; // null: general

    Allocator* allocator() const { return alloc ? alloc : mem_general(); }

    void reserve(u32 n) {
        if (n <= cap) return;
        u32 c = cap ? cap : 8;
        while (c < n) c *= 2;
        Allocator* a = allocator();
        u32 align = alignof(T) > KV_MEM_DEFAULT_ALIGN ? alignof(T) : KV_MEM_DEFAULT_ALIGN;
        T* nd = (T*)a->alloc(a, (usize)c * sizeof(T), align);
        KV_ASSERT(nd);
        if (len) __builtin_memcpy(nd, data, (usize)len * sizeof(T));
        if (data) a->free(a, data, (usize)cap * sizeof(T));
        data = nd;
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
        KV_VERIFY(len > 0);
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
        if (data) {
            Allocator* a = allocator();
            a->free(a, data, (usize)cap * sizeof(T));
        }
        data = nullptr;
        len = cap = 0;
    }

    T&       operator[](u32 i)       { KV_VERIFY(i < len); return data[i]; }
    const T& operator[](u32 i) const { KV_VERIFY(i < len); return data[i]; }
    T&       last()                  { KV_VERIFY(len > 0); return data[len - 1]; }
    T*       begin()       { return data; }
    T*       end()         { return data + len; }
    const T* begin() const { return data; }
    const T* end()   const { return data + len; }
    bool     empty() const { return len == 0; }

    Span<T>       span()       { return Span<T>(data, len); }
    Span<const T> span() const { return Span<const T>(data, len); }
};

template <class T>
Array<T> array_in(Allocator* a) {
    Array<T> arr;
    arr.alloc = a;
    return arr;
}

} // namespace kv
