// Korvid core: the fundamental types every other header uses: sized integers and floats
// (u32, f32, ...), Span, the two asserts and the build configuration.
//
// Engine code is written without exceptions, RTTI or the STL, and includes C headers
// only. A failure is a return value (null, false, a null handle), usually logged where
// it happens; an assert is for what can't happen in correct code.
#pragma once
#include <stdint.h>
#include <stddef.h>

// The three build configurations kormake builds (`kormake debug`, `dev`, `ship`):
//   KV_DEBUG  -O0, everything checked
//   KV_DEV    optimized, the build people run all day: cheap invariants stay on
//             (ownership, alignment, canaries, poison, the scheduler's access checks)
//   ship      neither: the checks compile away
// KV_DEBUG implies KV_DEV. Test them with #if, never #ifdef.
#ifndef KV_DEBUG
#define KV_DEBUG 0
#endif
#ifndef KV_DEV
#define KV_DEV KV_DEBUG
#endif

// Asserts print the expression, file and line, then abort. KV_ASSERT is checked in debug
// builds only, for checks too costly to run all day; KV_VERIFY in debug and dev, so it
// is the usual choice. Neither may have side effects the program relies on: ship drops
// both.
namespace kv { [[noreturn]] void assert_fail(const char* expr, const char* file, int line); }

#if KV_DEBUG
#define KV_ASSERT(x) do { if (!(x)) ::kv::assert_fail(#x, __FILE__, __LINE__); } while (0)
#else
#define KV_ASSERT(x) ((void)0)
#endif
#if KV_DEV
#define KV_VERIFY(x) do { if (!(x)) ::kv::assert_fail(#x, __FILE__, __LINE__); } while (0)
#else
#define KV_VERIFY(x) ((void)0)
#endif

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

// A view over contiguous data: a pointer and a count. It never owns or frees what it
// points at, so it is valid as long as the storage is. Indexing is bounds-checked in
// debug and dev. A Span<T> converts to a Span<const T>, and a C array converts to a
// Span of its length, so tables declared as arrays pass straight into a Span parameter:
//   constinit const SystemDesc k_systems[] = {...};   // .systems = k_systems
//   for (const RockHit& h : hits) ...                  // hits: Span<const RockHit>
template <class T>
struct Span {
    T*  data = nullptr;
    u32 len  = 0;

    constexpr Span() = default;
    constexpr Span(T* d, u32 n) : data(d), len(n) {}
    template <u32 N>
    constexpr Span(T (&arr)[N]) : data(arr), len(N) {}
    // Span<T> -> Span<const T>, and nothing else.
    template <class U, class = decltype((T*)(U*)nullptr)>
    constexpr Span(Span<U> o) : data(o.data), len(o.len) {}

    T&       operator[](u32 i)       { KV_VERIFY(i < len); return data[i]; }
    const T& operator[](u32 i) const { KV_VERIFY(i < len); return data[i]; }
    T*       begin()       { return data; }
    T*       end()         { return data + len; }
    const T* begin() const { return data; }
    const T* end()   const { return data + len; }
    bool     empty() const { return len == 0; }
};

} // namespace kv

// The element count of a C array, as a u32. Not for pointers.
#define KV_COUNTOF(a) ((::kv::u32)(sizeof(a) / sizeof((a)[0])))
// Marks a parameter or variable as deliberately unused, for -Werror builds.
#define KV_UNUSED(x) ((void)(x))

// Compiler specifics, in one place; code elsewhere names these, never a compiler's own.
// KV_PRINTF(f, a): parameter f is a printf format and its arguments start at a (1-based;
// a member function's this counts). gcc and clang check every call; MSVC has nothing
// equivalent outside /analyze, so there it says nothing. KV_RETURN_ADDRESS(): where the
// current function returns to, for attributing an allocation to its caller.
// KV_CTZ64(x): the index of the lowest set bit of a nonzero u64.
#if defined(_MSC_VER) && !defined(__clang__)
#define KV_COMPILER_MSVC 1
#define KV_PRINTF(f, a)
extern "C" void* _ReturnAddress(void);
extern "C" unsigned char _BitScanForward64(unsigned long* index, unsigned __int64 mask);
#pragma intrinsic(_ReturnAddress, _BitScanForward64)
#define KV_RETURN_ADDRESS() _ReturnAddress()
namespace kv { inline u32 ctz64(u64 x) { unsigned long i; _BitScanForward64(&i, x); return (u32)i; } }
#define KV_CTZ64(x) ::kv::ctz64(x)
#else
#define KV_COMPILER_MSVC 0
#define KV_PRINTF(f, a) __attribute__((format(printf, f, a)))
#define KV_RETURN_ADDRESS() __builtin_return_address(0)
#define KV_CTZ64(x) ((::kv::u32)__builtin_ctzll(x))
#endif
