#include "korvid/core/name.h"
#include "korvid/core/atomic.h"
#include "korvid/core/log.h"
#include <string.h>

namespace kv {

bool NameTable::init(Allocator* map_storage, usize string_reserve, const char* label) {
    offsets.init(map_storage);
    MemTag tag;
    if (map_storage) tag = map_storage->tag;
    return arena_create(strings, string_reserve, label, tag);
}

void NameTable::free() {
    offsets.free();
    arena_destroy(strings);
    count = 0;
}

Name NameTable::intern(const char* s) { return intern(s, s ? strlen(s) : 0); }

Name NameTable::intern(const char* s, usize len) {
    if (!s) return Name{};
    u64 h = hash_bytes(s, len);
    if (h == 0) h = 1; // keep 0 for the null name
    if (const u32* off = offsets.find(h)) {
        const char* have = (const char*)strings.base + *off;
        if (strlen(have) != len || memcmp(have, s, len) != 0) {
            KV_ERROR("name", "64-bit hash collision: '%s' and '%.*s'", have, (int)len, s);
            KV_VERIFY(!"name hash collision");
        }
        return Name{h};
    }
    char* copy = (char*)arena_alloc(strings, len + 1, 1);
    if (!copy) { KV_ERROR("name", "name table is full"); return Name{}; }
    memcpy(copy, s, len);
    copy[len] = 0;
    offsets.set(h, (u32)(copy - (char*)strings.base));
    ++count;
    return Name{h};
}

Name NameTable::lookup(const char* s) const {
    if (!s) return Name{};
    u64 h = hash_bytes(s, strlen(s));
    if (h == 0) h = 1;
    return offsets.has(h) ? Name{h} : Name{};
}

const char* NameTable::str(Name n) const {
    const u32* off = offsets.find(n.hash);
    return off ? (const char*)strings.base + *off : "";
}

// ---- the process table --------------------------------------------------------------

namespace {
NameTable g_names;
Allocator g_names_alloc; // the engine's, whoever interns first
bool      g_names_ready = false;
u32       g_names_lock  = 0;

void lock()   { while (kv::atomic_exchange(&g_names_lock, 1u, kv::MO_ACQUIRE)) {} }
void unlock() { kv::atomic_store(&g_names_lock, 0u, kv::MO_RELEASE); }

NameTable& table() {
    if (!g_names_ready) {
        mem_general_tagged(g_names_alloc, MemTag{}, "names");
        g_names.init(&g_names_alloc, 16u << 20, "names");
        g_names_ready = true;
    }
    return g_names;
}
} // namespace

void names_shutdown() {
    lock();
    if (g_names_ready) {
        g_names.free();
        g_names_ready = false;
    }
    unlock();
}

Name name_intern(const char* s) { return name_intern(s, s ? strlen(s) : 0); }

Name name_intern(const char* s, usize len) {
    lock();
    Name n = table().intern(s, len);
    unlock();
    return n;
}

Name name_lookup(const char* s) {
    lock();
    Name n = table().lookup(s);
    unlock();
    return n;
}

const char* name_str(Name n) {
    lock();
    const char* s = table().str(n);
    unlock();
    return s;
}

} // namespace kv
