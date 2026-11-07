#include "korvid/core/kind.h"
#include "korvid/core/hash.h"
#include <stdio.h>
#include <string.h>

namespace kv {

namespace {

void* table_resolve(kv_handle h, void* ctx) {
    Table& t = *(Table*)ctx;
    Handle hh{h};
    if (!t.valid(hh) || t.columns.len == 0) return nullptr;
    const Column& c = t.columns[0];
    return c.data + (usize)hh.index() * c.stride;
}

int table_describe(kv_handle h, void* ctx, char* out, size_t cap) {
    return ((const Table*)ctx)->describe(Handle{h}, out, cap);
}

} // namespace

bool Table::init(const KindDesc& d, Span<const ComponentDesc* const> comps, u32 table_index, u32 module_index, MemTag t) {
    KV_ASSERT(!desc);
    desc   = &d;
    index  = table_index;
    module = module_index;
    tag    = t;
    columns.alloc = mem_general();
    columns.reserve(comps.len); // never grows again: resource names are pointed at
    for (const ComponentDesc* cd : comps) {
        Column c;
        c.desc   = cd;
        c.stride = (cd->size + cd->align - 1) / cd->align * cd->align;
        usize bytes = os_page_align((usize)d.capacity * c.stride);
        c.data = (u8*)os_reserve(bytes);
        if (!c.data) { KV_ERROR("kind", "'%s': cannot reserve %llu bytes for %s", d.name, (unsigned long long)bytes, cd->name); return false; }
        if (cd->has_prev()) {
            c.prev = (u8*)os_reserve(bytes);
            if (!c.prev) { KV_ERROR("kind", "'%s': cannot reserve %llu bytes for %s (prev)", d.name, (unsigned long long)bytes, cd->name); return false; }
        }
        snprintf(c.resource_name, sizeof c.resource_name, "%s.%s", d.name, cd->name);
        columns.push(c);
    }
    kv_handle_kind_desc kd{};
    kd.name     = d.name;
    kd.type_key = comps.len ? comps[0]->type : nullptr; // a blind resolve is the first component's row
    kd.ctx      = this;
    kd.resolve  = table_resolve;
    kd.describe = table_describe;
    kd.flags    = KV_HANDLE_GENERATIONAL;
    kind = handle_kind_register(kd);
    if (!kind) return false;
    slots.use_kind(kind);
    return slots.init(d.capacity, t);
}

void Table::free() {
    if (kind) kv_handle_kind_unregister(kind);
    for (Column& c : columns) {
        usize bytes = os_page_align((usize)desc->capacity * c.stride);
        usize committed = os_page_align((usize)committed_rows * c.stride);
        if (c.data) { mem_account_decommit(tag, committed); os_release(c.data, bytes); }
        if (c.prev) { mem_account_decommit(tag, committed); os_release(c.prev, bytes); }
    }
    columns.free();
    slots.free();
    *this = Table{};
}

const Column* Table::column(u32 component_id) const {
    if (!component_id) return nullptr;
    for (const Column& c : columns)
        if (*c.desc->id == component_id) return &c;
    return nullptr;
}

// Commits the pool's growth steps for every column: 1024 rows, then doubling.
bool Table::ensure_rows(u32 n) {
    while (committed_rows < n) {
        u32 grow = committed_rows ? committed_rows : 1024;
        if (committed_rows + grow > desc->capacity) grow = desc->capacity - committed_rows;
        if (grow == 0) return false;
        u32 to = committed_rows + grow;
        usize total = 0;
        for (Column& c : columns) {
            usize have = os_page_align((usize)committed_rows * c.stride);
            usize need = os_page_align((usize)to * c.stride);
            if (need <= have) continue;
            if (!os_commit(c.data + have, need - have)) { KV_ERROR("kind", "'%s': commit failed", desc->name); return false; }
            total += need - have;
            if (c.prev) {
                if (!os_commit(c.prev + have, need - have)) { KV_ERROR("kind", "'%s': commit failed", desc->name); return false; }
                total += need - have;
            }
        }
        committed_rows = to;
        mem_account_commit(tag, total);
    }
    return true;
}

Handle Table::spawn() {
    if (slots.live >= desc->capacity) {
        KV_ERROR("kind", "'%s' is full (%u entities)", desc->name, desc->capacity);
        return Handle{};
    }
    Handle h = slots.create(0);
    if (h.is_null()) return h;
    u32 i = h.index();
    if (i >= committed_rows && !ensure_rows(i + 1)) { slots.destroy(h); return Handle{}; }
    for (Column& c : columns) {
        memset(row(c, i), 0, c.stride);
        if (c.prev) memset(c.prev + (usize)i * c.stride, 0, c.stride);
    }
    return h;
}

bool Table::destroy(Handle h) {
    if (!slots.destroy(h)) return false;
#if KV_DEV
    for (Column& c : columns) {
        memset(row(c, h.index()), 0xDD, c.stride); // a stale pointer sees poison
        if (c.prev) memset(c.prev + (usize)h.index() * c.stride, 0xDD, c.stride);
    }
#endif
    return true;
}

void Table::begin_tick() {
    u32 n = rows();
    for (Column& c : columns)
        if (c.prev) memcpy(c.prev, c.data, (usize)n * c.stride);
}

u64 Table::hash() const {
    u64 h = hash_str(desc->name);
    u32 n = rows();
    for (u32 i = 0; i < n; ++i) {
        if (!is_alive(i)) continue;
        h = hash_mix(h ^ handle_at(i).bits);
        for (const Column& c : columns)
            if (c.desc->is_sim()) h = hash_mix(h ^ component_hash(*c.desc, row(c, i)));
    }
    return h;
}

int Table::describe(Handle h, char* out, usize cap) const {
    if (!valid(h))
        return snprintf(out, cap, "%s (%u live)", h.index() < rows() ? "stale" : "out of range", live());
    usize at = 0;
    for (u32 k = 0; k < columns.len; ++k) {
        const Column& c = columns[k];
        if (k && at < cap) { out[at] = ' '; ++at; if (at < cap) out[at] = 0; }
        int n = component_describe(*c.desc, row(c, h.index()), out + (at < cap ? at : cap), at < cap ? cap - at : 0);
        if (n > 0) at += (usize)n;
    }
    return (int)at;
}

} // namespace kv
