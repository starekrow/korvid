// Korvid core: Array, the growable array of plain data.
//
// Like a vector of trivially copyable T, without the constructors: copying an Array
// copies the pointer, not the elements, and nothing frees it but free(). It allocates
// through an Allocator*: null means the general allocator, and array_in(alloc) makes one
// that lives in an arena or the scratch, where free() costs nothing. Growth doubles and
// moves the elements, so a pointer or Span into an Array lasts until the next push.
//
//   Array<Handle> targets = array_in<Handle>(mem_scratch());   // a frame's worth
//   targets.push(h);
//   for (Handle t : targets) ...
//
//   g->trail.alloc = &self.general;   // in module state: charged to the module
//   g->trail.free();                  // in shutdown
//
// push stops the program if the allocator refuses (a heap at its hard limit), so a
// caller never writes through null; try_push and try_push_n return null instead, for a
// caller that can do without the element.
#pragma once
#include "korvid/core/memory.h"
#include <string.h>

namespace kv {

// An Array could not grow where it has no way to say so (push, push_n, resize_fill): its
// allocator refused (a heap at its hard limit). Logged with the heap and the size, and
// stopped, in every build: never a write through null. A caller that can do without the
// element asks with try_push or try_push_n, which return null instead.
[[noreturn]] void array_grow_failed(const Allocator* a, usize bytes);

template <class T>
struct Array {
    static_assert(__is_trivially_copyable(T), "Array holds plain data only");

    // The fields are public: data[0, len) are the elements, cap what is allocated.

    T*         data  = nullptr;
    u32        len   = 0;
    u32        cap   = 0;
    Allocator* alloc = nullptr; // null: general

    Allocator* allocator() const { return alloc ? alloc : mem_general(); }

    // Room for n without another allocation. False if the allocator refused (a heap at its
    // hard limit): the array is as it was, its data and capacity untouched.
    bool reserve(u32 n) {
        if (n <= cap) return true;
        u32 c = cap ? cap : 8;
        while (c < n) c = c > 0x7FFFFFFFu ? n : c * 2;
        Allocator* a = allocator();
        u32 align = alignof(T) > KV_MEM_DEFAULT_ALIGN ? alignof(T) : KV_MEM_DEFAULT_ALIGN;
        T* nd = (T*)a->alloc(a, (usize)c * sizeof(T), align);
        if (!nd) return false;
        if (len) memcpy(nd, data, (usize)len * sizeof(T));
        if (data) a->free(a, data, (usize)cap * sizeof(T));
        data = nd;
        cap  = c;
        return true;
    }

    // Appends v. Out of memory it stops (array_grow_failed): for what the caller cannot do
    // without. try_push is the recoverable form.
    T& push(const T& v) {
        if (!reserve(len + 1)) array_grow_failed(allocator(), ((usize)len + 1) * sizeof(T));
        data[len] = v;
        return data[len++];
    }

    // Appends v and returns its slot, or null if the array could not grow (nothing changes).
    T* try_push(const T& v) {
        if (!reserve(len + 1)) return nullptr;
        data[len] = v;
        return &data[len++];
    }

    // Appends n uninitialized slots and returns a pointer to the first. Out of memory it
    // stops, as push does; try_push_n returns null instead.
    T* push_n(u32 n) {
        if (!reserve(len + n)) array_grow_failed(allocator(), ((usize)len + n) * sizeof(T));
        T* p = data + len;
        len += n;
        return p;
    }

    T* try_push_n(u32 n) {
        if (!reserve(len + n)) return nullptr;
        T* p = data + len;
        len += n;
        return p;
    }

    // Removes and returns the last element; the array must not be empty.
    T pop() {
        KV_VERIFY(len > 0);
        return data[--len];
    }

    // Grows with copies of v until len == n. Never shrinks.
    void resize_fill(u32 n, const T& v) {
        if (n <= len) return;
        if (!reserve(n)) array_grow_failed(allocator(), (usize)n * sizeof(T));
        for (u32 i = len; i < n; ++i) data[i] = v;
        len = n;
    }

    // Empties the array and keeps its memory, for refilling.
    void clear() { len = 0; }

    // Gives the memory back to the allocator. Call it when the owner is done (a module's
    // shutdown); in an arena it costs nothing, since the arena frees it all at once.
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
    T&       last()                  { KV_VERIFY(len > 0); return data[len - 1]; } // the array must not be empty
    T*       begin()       { return data; }
    T*       end()         { return data + len; }
    const T* begin() const { return data; }
    const T* end()   const { return data + len; }
    bool     empty() const { return len == 0; }

    // The elements as a Span, to pass where a view is wanted. Valid until the array grows.
    Span<T>       span()       { return Span<T>(data, len); }
    Span<const T> span() const { return Span<const T>(data, len); }
};

// An empty array that allocates from `a`: an arena, mem_scratch(), a module's general.
template <class T>
Array<T> array_in(Allocator* a) {
    Array<T> arr;
    arr.alloc = a;
    return arr;
}

} // namespace kv
