// Korvid platform: the table of what the OS has open for the process, and the handle
// kinds the platform issues for it (os_handle.h, docs/design/handles.md).
//
// One table for the process, made at its first use: files are opened before any engine
// exists and from threads that belong to no module. It is address space reserved once and
// committed as it fills (the page layer, so it is none of the heap's and no leak check
// counts it), so a slot never moves. Making and releasing take the lock; reading a slot
// does not: its generation word says whether it is the one the handle names.
#include "os_handle_table.h"
#include "korvid/core/atomic.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/os_memory.h"
#include "korvid/core/os_thread.h"
#include "korvid/core/time.h"
#include <string.h>

namespace kv {

namespace {

#define KV_OS_HANDLE_INFO (KV_DEV || KV_DEBUG)

constexpr u32 kMaxSlots = 1u << 16;
constexpr u32 kAlive    = 0x80000000u;
constexpr u32 kNone     = 0xFFFFFFFFu;
constexpr u32 kKinds    = 6;

struct OsSlot {
    i64 os;         // the OS's value while alive: atomic (read without the lock)
    u32 gen;        // the generation, with kAlive while it is open: atomic
    u32 next_free;  // the free list, oldest first
    u8  kind;       // OsKind: atomic, as os
    u32 flags;      // OsHandleFlag bits: atomic
#if KV_OS_HANDLE_INFO
    u16  by;        // mem_owner() at the open: the module index + 1, 0 outside any
    f64  opened_at;
    u64  in, out;   // atomic
    char what[160];
#endif
};

struct Table {
    OsMutex lock;
    OsSlot* slots     = nullptr;
    u32     len       = 0;     // slots ever used: atomic, only grows
    u32     committed = 0;     // slots whose pages are committed
    u32     live      = 0;     // atomic
    u32     free_head = kNone, free_tail = kNone;
    u16     kind_id[kKinds] = {}; // the engine's handle kind for each OsKind
};

Table g_table;
u32   g_ready = 0; // 0: not made, 1: being made, 2: made. Atomic.

const char* const k_kind_names[kKinds] = {"none", "file", "socket", "pipe", "notify", "process"};
const char* const k_handle_names[kKinds] = {"", "os.file", "os.socket", "os.pipe", "os.notify", "os.process"};

OsSlot* slot_of(Handle h) {
    if (h.is_null() || atomic_load(&g_ready, MO_ACQUIRE) != 2) return nullptr;
    u32 i = h.index();
    if (i >= atomic_load(&g_table.len, MO_ACQUIRE)) return nullptr;
    OsSlot& s = g_table.slots[i];
    if (atomic_load(&s.gen, MO_ACQUIRE) != (h.gen() | kAlive)) return nullptr;
    u8 kind = atomic_load(&s.kind, MO_RELAXED);
    if (kind >= kKinds || g_table.kind_id[kind] != h.kind()) return nullptr;
    return &s;
}

// A slot's value for `h`, read without the lock: the generation again after it, so a slot
// released (and made again for another descriptor) since slot_of looked gives -1, never
// what the OS has given someone else under the same number.
i64 slot_value(const OsSlot& s, Handle h) {
    i64 v = atomic_load(&s.os, MO_ACQUIRE);
    return atomic_load(&s.gen, MO_ACQUIRE) == (h.gen() | kAlive) ? v : -1;
}

void* resolve_slot(kv_handle h, void*) { return slot_of(Handle{h}); }

int describe_slot(kv_handle h, void*, char* out, size_t cap) {
    Handle hh{h};
    OsHandleInfo info;
    if (!os_handle_info(hh, &info)) return format(out, cap, "closed");
#if KV_OS_HANDLE_INFO
    return format(out, cap, "%s%s%s, %llu read, %llu written", info.what[0] ? info.what : "(unnamed)", info.by[0] ? ", opened by " : "", info.by,
                  (unsigned long long)info.bytes_in, (unsigned long long)info.bytes_out);
#else
    return format(out, cap, "open");
#endif
}

// The table and its kinds, once: whichever thread asks first makes them, the rest wait.
bool ensure() {
    if (atomic_load(&g_ready, MO_ACQUIRE) == 2) return true;
    u32 none = 0;
    if (atomic_compare_exchange(&g_ready, &none, 1u, false, MO_ACQ_REL, MO_ACQUIRE)) {
        os_mutex_init(&g_table.lock);
        g_table.slots = (OsSlot*)os_reserve((usize)kMaxSlots * sizeof(OsSlot));
        for (u32 k = 1; k < kKinds; ++k) {
            kv_handle_kind_desc d{};
            d.name     = k_handle_names[k];
            d.type_key = type_key<OsSlot>();
            d.resolve  = resolve_slot;
            d.describe = describe_slot;
            d.flags    = KV_HANDLE_GENERATIONAL;
            g_table.kind_id[k] = handle_kind_register(d);
        }
        atomic_store(&g_ready, 2u, MO_RELEASE);
        return g_table.slots != nullptr;
    }
    while (atomic_load(&g_ready, MO_ACQUIRE) != 2) time_sleep(0.0001);
    return g_table.slots != nullptr;
}

} // namespace

const char* os_kind_name(OsKind k) { return (u32)k < kKinds ? k_kind_names[(u32)k] : "?"; }

Handle os_handle_make(OsKind kind, i64 os, const char* what) {
    if (os < 0 || kind == OsKind::None || (u32)kind >= kKinds || !ensure()) return Handle{};
    Table& t = g_table;
    if (!t.kind_id[(u32)kind]) return Handle{};
    os_mutex_lock(&t.lock);
    u32 i;
    if (t.free_head != kNone) {
        i = t.free_head;
        t.free_head = t.slots[i].next_free;
        if (t.free_head == kNone) t.free_tail = kNone;
    } else {
        u32 len = atomic_load(&t.len, MO_RELAXED);
        if (len == kMaxSlots) {
            os_mutex_unlock(&t.lock);
            KV_ERROR("platform", "%u OS handles are open: no more can be tracked", kMaxSlots);
            return Handle{};
        }
        if (len == t.committed) {
            // A page run at a time: 256 slots, rounded to pages.
            usize from = os_page_align((usize)t.committed * sizeof(OsSlot));
            usize to   = os_page_align((usize)(t.committed + 256) * sizeof(OsSlot));
            if (!os_commit((u8*)t.slots + from, to - from)) {
                os_mutex_unlock(&t.lock);
                KV_ERROR("platform", "the OS handle table could not grow");
                return Handle{};
            }
            t.committed = (u32)(to / sizeof(OsSlot));
            if (t.committed > kMaxSlots) t.committed = kMaxSlots;
        }
        i = len;
        t.slots[i].gen = 1;
        atomic_store(&t.len, len + 1, MO_RELEASE);
    }
    OsSlot& s = t.slots[i];
    atomic_store(&s.os, os, MO_RELAXED);
    atomic_store(&s.kind, (u8)kind, MO_RELAXED); // both published by the generation's release below
    atomic_store(&s.flags, 0u, MO_RELAXED);
    s.next_free = kNone;
#if KV_OS_HANDLE_INFO
    s.by        = mem_owner();
    s.opened_at = time_now();
    s.in = s.out = 0;
    format(s.what, sizeof s.what, "%s", what ? what : "");
#else
    (void)what;
#endif
    u32 gen = s.gen & ~kAlive;
    atomic_store(&s.gen, gen | kAlive, MO_RELEASE);
    atomic_fetch_add(&t.live, 1u, MO_RELAXED);
    os_mutex_unlock(&t.lock);
    return Handle::make(t.kind_id[(u32)kind], gen, i);
}

i64 os_handle_value(Handle h) {
    OsSlot* s = slot_of(h);
    return s ? slot_value(*s, h) : -1;
}

i64 os_handle_value(Handle h, OsKind want) {
    OsSlot* s = slot_of(h);
    return s && atomic_load(&s->kind, MO_RELAXED) == (u8)want ? slot_value(*s, h) : -1;
}

i64 os_handle_release(Handle h) {
    if (h.is_null() || atomic_load(&g_ready, MO_ACQUIRE) != 2) return -1;
    Table& t = g_table;
    os_mutex_lock(&t.lock);
    OsSlot* s = slot_of(h); // again under the lock: two closers, one wins
    if (!s) { os_mutex_unlock(&t.lock); return -1; }
    i64 os = atomic_load(&s->os, MO_RELAXED);
    u32 i = h.index();
    u32 gen = (s->gen & ~kAlive) + 1;
    if (gen > KV_HANDLE_MAX_GEN) gen = 1; // skip 0 on wrap
    atomic_store(&s->gen, gen, MO_RELEASE); // dead: every copy of the handle is stale
    atomic_store(&s->os, (i64)-1, MO_RELAXED);
    // Appended: the slot comes back only after every other free one has been reused.
    s->next_free = kNone;
    if (t.free_tail == kNone) t.free_head = i;
    else t.slots[t.free_tail].next_free = i;
    t.free_tail = i;
    atomic_fetch_sub(&t.live, 1u, MO_RELAXED);
    os_mutex_unlock(&t.lock);
    return os;
}

void os_handle_count(Handle h, u64 in, u64 out) {
#if KV_OS_HANDLE_INFO
    if (OsSlot* s = slot_of(h)) {
        if (in) atomic_fetch_add(&s->in, in, MO_RELAXED);
        if (out) atomic_fetch_add(&s->out, out, MO_RELAXED);
    }
#else
    (void)h; (void)in; (void)out;
#endif
}

void os_handle_set_flag(Handle h, u32 flag, bool on) {
    OsSlot* s = slot_of(h);
    if (!s) return;
    u32 was = atomic_load(&s->flags, MO_RELAXED);
    for (;;) {
        u32 want = on ? (was | flag) : (was & ~flag);
        if (want == was || atomic_compare_exchange(&s->flags, &was, want, true, MO_RELEASE, MO_RELAXED)) return;
    }
}

bool os_handle_flag(Handle h, u32 flag) {
    OsSlot* s = slot_of(h);
    return s && (atomic_load(&s->flags, MO_ACQUIRE) & flag) != 0;
}

OsKind os_handle_kind(Handle h) {
    OsSlot* s = slot_of(h);
    return s ? (OsKind)atomic_load(&s->kind, MO_RELAXED) : OsKind::None;
}

bool os_handle_info(Handle h, OsHandleInfo* out) {
    *out = OsHandleInfo{};
    OsSlot* s = slot_of(h);
    if (!s) return false;
    out->kind = (OsKind)atomic_load(&s->kind, MO_RELAXED);
#if KV_OS_HANDLE_INFO
    format(out->what, sizeof out->what, "%s", s->what);
    out->opened_at = s->opened_at;
    out->bytes_in  = atomic_load(&s->in, MO_RELAXED);
    out->bytes_out = atomic_load(&s->out, MO_RELAXED);
    // Who opened it: the module whose code was running. Outside any (the program's own
    // main, a worker of the queue's, a thread of its own) it is the program's: its main
    // module, the last of the list, which is the executable's own.
    const char* const* names = mem_module_names();
    u32 count = mem_module_name_count();
    u32 by = s->by ? (u32)(s->by - 1) : count ? count - 1 : ~0u;
    if (names && by < count && names[by]) format(out->by, sizeof out->by, "%s", names[by]);
#endif
    return true;
}

u32 os_handles_open() { return atomic_load(&g_ready, MO_ACQUIRE) == 2 ? atomic_load(&g_table.live, MO_RELAXED) : 0; }

void os_handles_each(void (*fn)(void* user, Handle h, const OsHandleInfo& info), void* user) {
    if (atomic_load(&g_ready, MO_ACQUIRE) != 2) return;
    Table& t = g_table;
    os_mutex_lock(&t.lock);
    u32 len = atomic_load(&t.len, MO_RELAXED);
    for (u32 i = 0; i < len; ++i) {
        OsSlot& s = t.slots[i];
        if (!(s.gen & kAlive)) continue;
        Handle h = Handle::make(t.kind_id[atomic_load(&s.kind, MO_RELAXED)], s.gen & ~kAlive, i);
        OsHandleInfo info;
        if (os_handle_info(h, &info)) fn(user, h, info);
    }
    os_mutex_unlock(&t.lock);
}

} // namespace kv
