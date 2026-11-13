#include "korvid/core/handle.h"
#include "korvid/core/format.h"
#include "korvid/core/atomic.h"
#include "korvid/core/hash.h"
#include "korvid/core/log.h"
#include <string.h>

namespace {

constexpr uint32_t kMaxKinds = KV_HANDLE_MAX_KIND + 1; // ids 1..4095
constexpr uint32_t kNameLen  = 32;

// The registered copy of a descriptor, with its name stored alongside and pointed at.
struct Kind {
    kv_handle_kind_desc desc;
    char                name[kNameLen];
    uint64_t            name_hash; // of the stored name: what a kind is across runs
    uint32_t            live;
};

// Rows are written at registration and read lock-free afterwards; ids never come back.
// Registration is startup work, but the counter is atomic so two issuers starting at
// once cannot be handed the same id; a row becomes visible only when its live flag is.
Kind     g_kinds[kMaxKinds];
uint32_t g_count = 1; // 0 is reserved; atomic
uint32_t g_warned = 0;

uint32_t count() { return kv::atomic_load(&g_count, kv::MO_ACQUIRE); }

const Kind* kind_of(kv_handle h) {
    uint16_t k = kv_handle_kind(h);
    if (k == 0 || k >= count()) return nullptr;
    const Kind& kind = g_kinds[k];
    return kv::atomic_load(&kind.live, kv::MO_ACQUIRE) ? &kind : nullptr;
}

} // namespace

extern "C" {

uint16_t kv_handle_kind_register(const kv_handle_kind_desc* desc) {
    if (!desc || !desc->resolve) {
        KV_ERROR("handle", "kind registration needs a resolve function");
        return 0;
    }
    uint32_t id32 = kv::atomic_fetch_add(&g_count, 1u, kv::MO_ACQ_REL);
    if (id32 >= kMaxKinds) {
        kv::atomic_fetch_sub(&g_count, 1u, kv::MO_ACQ_REL);
        KV_ERROR("handle", "no kind ids left (%u used); ids are never reused within a run", kMaxKinds - 1);
        return 0;
    }
    uint16_t id = (uint16_t)id32;
    Kind& k = g_kinds[id];
    k.desc = *desc;
    strncpy(k.name, desc->name ? desc->name : "", kNameLen - 1);
    k.name[kNameLen - 1] = 0;
    k.desc.name = k.name;
    k.name_hash = kv::hash_str(k.name);
    kv::atomic_store(&k.live, 1u, kv::MO_RELEASE);
    if (id32 > (kMaxKinds * 3) / 4 && kv::atomic_exchange(&g_warned, 1u, kv::MO_RELAXED) == 0)
        KV_WARN("handle", "%u of %u kind ids used", id32, kMaxKinds - 1);
    return id;
}

void kv_handle_kind_unregister(uint16_t kind) {
    if (kind == 0 || kind >= count()) return;
    kv::atomic_store(&g_kinds[kind].live, 0u, kv::MO_RELEASE);
}

const kv_handle_kind_desc* kv_handle_kind_desc_get(uint16_t kind) {
    return kind != 0 && kind < count() ? &g_kinds[kind].desc : nullptr;
}

void kv_handle_kind_set_ctx(uint16_t kind, void* ctx) {
    if (kind == 0 || kind >= count()) return;
    kv::atomic_store(&g_kinds[kind].desc.ctx, ctx, kv::MO_RELEASE);
}

void* kv_handle_resolve(kv_handle h) {
    const Kind* k = kind_of(h);
    if (!k) return nullptr;
    if ((k->desc.flags & KV_HANDLE_GENERATIONAL) && kv_handle_gen(h) == 0) return nullptr;
    return k->desc.resolve(h, kv::atomic_load(&k->desc.ctx, kv::MO_ACQUIRE));
}

void* kv_handle_resolve_as(kv_handle h, const void* type_key) {
    const Kind* k = kind_of(h);
    if (!k || k->desc.type_key != type_key) return nullptr;
    if ((k->desc.flags & KV_HANDLE_GENERATIONAL) && kv_handle_gen(h) == 0) return nullptr;
    return k->desc.resolve(h, kv::atomic_load(&k->desc.ctx, kv::MO_ACQUIRE));
}

const char* kv_handle_kind_name(uint16_t kind) {
    return kind != 0 && kind < count() ? g_kinds[kind].name : "";
}

uint64_t kv_handle_kind_name_hash(uint16_t kind) {
    return kind != 0 && kind < count() ? g_kinds[kind].name_hash : 0;
}

uint16_t kv_handle_kind_find(const char* name) {
    if (!name || !name[0]) return 0;
    for (uint32_t k = count(); k-- > 1;) // the latest first: ids are never reused
        if (kv::atomic_load(&g_kinds[k].live, kv::MO_ACQUIRE) && strncmp(g_kinds[k].name, name, kNameLen - 1) == 0) return (uint16_t)k;
    return 0;
}

const void* kv_handle_type_key(kv_handle h) {
    const Kind* k = kind_of(h);
    return k ? k->desc.type_key : nullptr;
}

const char* kv_handle_name(kv_handle h) {
    const Kind* k = kind_of(h);
    if (!k || !k->desc.name_of) return nullptr;
    if ((k->desc.flags & KV_HANDLE_GENERATIONAL) && kv_handle_gen(h) == 0) return nullptr;
    return k->desc.name_of(h, kv::atomic_load(&k->desc.ctx, kv::MO_ACQUIRE));
}

kv_handle kv_handle_find(uint16_t kind, const char* name) {
    if (!name || !name[0] || kind == 0 || kind >= count()) return 0;
    const Kind& k = g_kinds[kind];
    if (!kv::atomic_load(&k.live, kv::MO_ACQUIRE) || !k.desc.find_name) return 0;
    return k.desc.find_name(name, kv::atomic_load(&k.desc.ctx, kv::MO_ACQUIRE));
}

int kv_handle_kind_names(uint16_t kind) {
    return kind != 0 && kind < count() && kv::atomic_load(&g_kinds[kind].live, kv::MO_ACQUIRE) && g_kinds[kind].desc.name_of != nullptr;
}

int kv_handle_kind_is_live(uint16_t kind) {
    return kind != 0 && kind < count() && kv::atomic_load(&g_kinds[kind].live, kv::MO_ACQUIRE);
}

uint32_t kv_handle_kind_count(void) { return count(); }

int kv_handle_describe(kv_handle h, char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    if (h == 0) return kv::format(out, cap, "null");
    uint16_t kind = kv_handle_kind(h);
    const Kind* k = kind_of(h);
    int n = kv::format(out, cap, "%s#%u:%u", k ? k->name : (kind < count() ? "(unregistered)" : "(unknown)"),
                     kv_handle_index(h), kv_handle_gen(h));
    if (n < 0) return 0;
    if ((size_t)n >= cap) return (int)cap - 1;
    if (k && k->desc.describe) {
        out[n++] = ' ';
        if ((size_t)n >= cap) { out[cap - 1] = 0; return (int)cap - 1; }
        int m = k->desc.describe(h, kv::atomic_load(&k->desc.ctx, kv::MO_ACQUIRE), out + n, cap - (size_t)n);
        if (m > 0) n += m;
        else out[--n] = 0;
    }
    return n;
}

} // extern "C"
