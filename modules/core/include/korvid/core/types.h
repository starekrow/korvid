// Korvid core: fundamental types, assert, compiler macros.
// Runtime rules: no exceptions, no RTTI, no STL. C headers only.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace kv {

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;
typedef float    f32;
typedef double   f64;
typedef size_t   usize;

// A view over contiguous plain data. Never owns.
template <class T>
struct Span {
    T*  data = nullptr;
    u32 len  = 0;

    Span() = default;
    Span(T* d, u32 n) : data(d), len(n) {}
    template <u32 N>
    Span(T (&arr)[N]) : data(arr), len(N) {}

    T&       operator[](u32 i)       { return data[i]; }
    const T& operator[](u32 i) const { return data[i]; }
    T*       begin()       { return data; }
    T*       end()         { return data + len; }
    const T* begin() const { return data; }
    const T* end()   const { return data + len; }
    bool     empty() const { return len == 0; }
};

[[noreturn]] void assert_fail(const char* expr, const char* file, int line);

} // namespace kv

#ifndef KV_DEBUG
#define KV_DEBUG 0
#endif

#if KV_DEBUG
#define KV_ASSERT(x) do { if (!(x)) ::kv::assert_fail(#x, __FILE__, __LINE__); } while (0)
#else
#define KV_ASSERT(x) ((void)0)
#endif

#define KV_COUNTOF(a) ((::kv::u32)(sizeof(a) / sizeof((a)[0])))
#define KV_UNUSED(x) ((void)(x))
