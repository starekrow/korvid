// Korvid core: atomic operations on plain variables, with explicit memory orders. Shared
// data stays an ordinary integer, pointer or enum of 1, 2, 4 or 8 bytes; every access that
// can race goes through one of these. gcc and clang map them to their __atomic builtins;
// MSVC to its interlocked intrinsics (x64 only, so far: its loads and stores are ordered
// by the hardware, and the compiler barriers keep cl from moving them).
#pragma once
#include "korvid/core/types.h"
#if KV_COMPILER_MSVC
#include <intrin.h>
#include <string.h>
#if !defined(_M_X64)
#error "korvid atomics: MSVC is supported on x64 only so far"
#endif
#endif

namespace kv {

enum MemOrder { MO_RELAXED, MO_ACQUIRE, MO_RELEASE, MO_ACQ_REL, MO_SEQ_CST };

// The value argument takes the variable's type rather than deducing one of its own, so
// atomic_add_fetch(&count_u64, 1, ...) is fine.
template <class T> struct AtomicArg { typedef T type; };

#if !KV_COMPILER_MSVC

namespace atomic_detail {
constexpr int order(MemOrder o) {
    return o == MO_RELAXED ? __ATOMIC_RELAXED : o == MO_ACQUIRE ? __ATOMIC_ACQUIRE : o == MO_RELEASE ? __ATOMIC_RELEASE
         : o == MO_ACQ_REL ? __ATOMIC_ACQ_REL : __ATOMIC_SEQ_CST;
}
} // namespace atomic_detail

template <class T> inline T atomic_load(const T* p, MemOrder o) { return __atomic_load_n(p, atomic_detail::order(o)); }
template <class T> inline void atomic_store(T* p, typename AtomicArg<T>::type v, MemOrder o) { __atomic_store_n(p, v, atomic_detail::order(o)); }
template <class T> inline T atomic_exchange(T* p, typename AtomicArg<T>::type v, MemOrder o) { return __atomic_exchange_n(p, v, atomic_detail::order(o)); }
template <class T> inline bool atomic_compare_exchange(T* p, T* expected, typename AtomicArg<T>::type desired, bool weak,
                                                       MemOrder success, MemOrder failure) {
    return __atomic_compare_exchange_n(p, expected, desired, weak, atomic_detail::order(success), atomic_detail::order(failure));
}
template <class T> inline T atomic_fetch_add(T* p, typename AtomicArg<T>::type v, MemOrder o) { return __atomic_fetch_add(p, v, atomic_detail::order(o)); }
template <class T> inline T atomic_fetch_sub(T* p, typename AtomicArg<T>::type v, MemOrder o) { return __atomic_fetch_sub(p, v, atomic_detail::order(o)); }
template <class T> inline T atomic_add_fetch(T* p, typename AtomicArg<T>::type v, MemOrder o) { return __atomic_add_fetch(p, v, atomic_detail::order(o)); }
template <class T> inline T atomic_sub_fetch(T* p, typename AtomicArg<T>::type v, MemOrder o) { return __atomic_sub_fetch(p, v, atomic_detail::order(o)); }

// A fence: orders this thread's memory accesses around it without an atomic of its own.
// The case that needs one: data read with plain loads and then validated by re-reading an
// index (a seqlock, a ring that may lap its reader). An acquire load keeps later accesses
// after it, not earlier ones; an acquire fence between the data and the re-read keeps the
// data's loads before it. x64 never reorders loads with loads, arm64 does.
inline void atomic_fence(MemOrder o) { __atomic_thread_fence(atomic_detail::order(o)); }

#else

namespace atomic_detail {
template <usize N> struct Int;
template <> struct Int<1> { typedef char type; };
template <> struct Int<2> { typedef short type; };
template <> struct Int<4> { typedef long type; };
template <> struct Int<8> { typedef __int64 type; };
template <class T> using Bits = typename Int<sizeof(T)>::type;

template <class T> inline Bits<T> bits(T v) { Bits<T> b; memcpy(&b, &v, sizeof v); return b; }
template <class T> inline T value(Bits<T> b) { T v; memcpy(&v, &b, sizeof v); return v; }
template <class T> inline volatile Bits<T>* at(const T* p) { return reinterpret_cast<volatile Bits<T>*>(const_cast<T*>(p)); }

// Each interlocked intrinsic is a full barrier, which satisfies every order.
inline char    exchange(volatile char* p, char v)       { return _InterlockedExchange8(p, v); }
inline short   exchange(volatile short* p, short v)     { return _InterlockedExchange16(p, v); }
inline long    exchange(volatile long* p, long v)       { return _InterlockedExchange(p, v); }
inline __int64 exchange(volatile __int64* p, __int64 v) { return _InterlockedExchange64(p, v); }
inline char    cas(volatile char* p, char d, char e)          { return _InterlockedCompareExchange8(p, d, e); }
inline short   cas(volatile short* p, short d, short e)       { return _InterlockedCompareExchange16(p, d, e); }
inline long    cas(volatile long* p, long d, long e)          { return _InterlockedCompareExchange(p, d, e); }
inline __int64 cas(volatile __int64* p, __int64 d, __int64 e) { return _InterlockedCompareExchange64(p, d, e); }
inline char    add(volatile char* p, char v)       { return _InterlockedExchangeAdd8(p, v); }
inline short   add(volatile short* p, short v)     { return _InterlockedExchangeAdd16(p, v); }
inline long    add(volatile long* p, long v)       { return _InterlockedExchangeAdd(p, v); }
inline __int64 add(volatile __int64* p, __int64 v) { return _InterlockedExchangeAdd64(p, v); }
} // namespace atomic_detail

// On x64 a plain load already acquires and a plain store already releases; the compiler
// barriers stop cl reordering around them. A sequentially consistent store is an exchange.
template <class T> inline T atomic_load(const T* p, MemOrder) {
    atomic_detail::Bits<T> b = *atomic_detail::at(p);
    _ReadWriteBarrier();
    return atomic_detail::value<T>(b);
}
template <class T> inline void atomic_store(T* p, typename AtomicArg<T>::type v, MemOrder o) {
    if (o == MO_SEQ_CST) { atomic_detail::exchange(atomic_detail::at(p), atomic_detail::bits<T>(v)); return; }
    _ReadWriteBarrier();
    *atomic_detail::at(p) = atomic_detail::bits<T>(v);
    _ReadWriteBarrier();
}
template <class T> inline T atomic_exchange(T* p, typename AtomicArg<T>::type v, MemOrder) {
    return atomic_detail::value<T>(atomic_detail::exchange(atomic_detail::at(p), atomic_detail::bits<T>(v)));
}
template <class T> inline bool atomic_compare_exchange(T* p, T* expected, typename AtomicArg<T>::type desired, bool,
                                                       MemOrder, MemOrder) {
    atomic_detail::Bits<T> want = atomic_detail::bits<T>(*expected);
    atomic_detail::Bits<T> old = atomic_detail::cas(atomic_detail::at(p), atomic_detail::bits<T>(desired), want);
    if (old == want) return true;
    *expected = atomic_detail::value<T>(old);
    return false;
}
// Arithmetic is for integers; it wraps as unsigned arithmetic does.
template <class T> inline T atomic_fetch_add(T* p, typename AtomicArg<T>::type v, MemOrder) {
    return (T)atomic_detail::add(atomic_detail::at(p), (atomic_detail::Bits<T>)v);
}
template <class T> inline T atomic_fetch_sub(T* p, typename AtomicArg<T>::type v, MemOrder o) { return atomic_fetch_add(p, (T)(T(0) - v), o); }
template <class T> inline T atomic_add_fetch(T* p, typename AtomicArg<T>::type v, MemOrder o) { return (T)(atomic_fetch_add(p, v, o) + v); }
template <class T> inline T atomic_sub_fetch(T* p, typename AtomicArg<T>::type v, MemOrder o) { return (T)(atomic_fetch_sub(p, v, o) - v); }

// x64 orders loads with loads and stores with stores, so acquire and release fences are
// compiler barriers; only a sequentially consistent one (a store, then a load) needs mfence.
inline void atomic_fence(MemOrder o) {
    if (o == MO_SEQ_CST) _mm_mfence();
    else _ReadWriteBarrier();
}

#endif

} // namespace kv
