#include "korvid/core/kind.h"
#include "korvid/core/format.h"
#include "korvid/core/hash.h"
#include "korvid/core/memory.h"
#include "korvid/core/profile.h"
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

const char* table_name_of(kv_handle h, void* ctx) {
    Name n = ((const Table*)ctx)->name_of(Handle{h});
    return n.is_null() ? nullptr : name_str(n);
}

kv_handle table_find_name(const char* name, void* ctx) {
    const Table& t = *(const Table*)ctx;
    Name n = name_lookup(name);
    if (n.is_null()) return 0;
    for (u32 i = 0; i < t.names.count(); ++i)
        if (t.sparse_live(t.names, i) && ((const Name*)t.names.at(i))->hash == n.hash) return t.handle_at(t.names.slots[i]).bits;
    return 0;
}

} // namespace

u32 SparseRows::find(u32 slot) const {
    u32 lo = 0, hi = slots.len;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (slots.data[mid] < slot) lo = mid + 1; else hi = mid;
    }
    return lo < slots.len && slots.data[lo] == slot ? lo : KV_POOL_NONE;
}

u8* SparseRows::insert(u32 slot, u32 gen) {
    u32 lo = 0, hi = slots.len;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (slots.data[mid] < slot) lo = mid + 1; else hi = mid;
    }
    if (lo == slots.len || slots.data[lo] != slot) {
        slots.push(0); gens.push(0); data.push_n(stride);
        u32 tail = slots.len - 1 - lo;
        memmove(slots.data + lo + 1, slots.data + lo, (usize)tail * sizeof(u32));
        memmove(gens.data + lo + 1, gens.data + lo, (usize)tail * sizeof(u32));
        memmove(data.data + (usize)(lo + 1) * stride, data.data + (usize)lo * stride, (usize)tail * stride);
        slots.data[lo] = slot;
    }
    gens.data[lo] = gen;
    memset(at(lo), 0, stride);
    return at(lo);
}

void SparseRows::erase(u32 i) {
    KV_VERIFY(i < slots.len);
    u32 tail = slots.len - 1 - i;
    memmove(slots.data + i, slots.data + i + 1, (usize)tail * sizeof(u32));
    memmove(gens.data + i, gens.data + i + 1, (usize)tail * sizeof(u32));
    memmove(data.data + (usize)i * stride, data.data + (usize)(i + 1) * stride, (usize)tail * stride);
    --slots.len; --gens.len; data.len -= stride;
}

const char* prop_type_name(PropType t) {
    static const char* const k_names[] = {"none", "int", "float", "bool", "string", "handle"};
    return (u32)t < KV_COUNTOF(k_names) ? k_names[(u32)t] : "?";
}

bool Table::init(const KindDesc& d, const char* full_name, Span<const ComponentDesc* const> comps, u32 table_index, u32 module_index, MemTag t,
                 Span<const ComponentDesc* const> attachable, u32 capacity_override) {
    KV_ASSERT(!desc);
    desc   = &d;
    capacity = capacity_override ? capacity_override : d.capacity;
    name   = full_name;
    index  = table_index;
    module = module_index;
    tag    = t;
    columns.alloc = mem_general();
    names.stride  = sizeof(Name);
    attachments.alloc = mem_general();
    attachments.reserve(attachable.len ? attachable.len : 1); // never grows again: resource names are pointed at
    for (const ComponentDesc* cd : attachable) {
        Attachment& a = attachments.push(Attachment{});
        a.desc = cd;
        a.rows.stride = (cd->size + cd->align - 1) / cd->align * cd->align;
        format(a.resource_name, sizeof a.resource_name, "%s.@%s", full_name, cd->name);
    }
    if (d.props) format(props_resource_name, sizeof props_resource_name, "%s.@props", full_name);
    columns.reserve(comps.len); // never grows again: resource names are pointed at
    for (const ComponentDesc* cd : comps) {
        Column c;
        c.desc   = cd;
        c.stride = (cd->size + cd->align - 1) / cd->align * cd->align;
        usize bytes = os_page_align((usize)capacity * c.stride);
        c.data = (u8*)os_reserve(bytes);
        if (!c.data) { KV_ERROR("kind", "'%s': cannot reserve %llu bytes for %s", name, (unsigned long long)bytes, cd->name); return false; }
        if (cd->has_prev()) {
            c.prev = (u8*)os_reserve(bytes);
            if (!c.prev) { KV_ERROR("kind", "'%s': cannot reserve %llu bytes for %s (prev)", name, (unsigned long long)bytes, cd->name); return false; }
        }
        format(c.resource_name, sizeof c.resource_name, "%s.%s", name, cd->name);
        columns.push(c);
    }
    kv_handle_kind_desc kd{};
    kd.name     = name;
    kd.type_key = comps.len ? comps[0]->type : nullptr; // a blind resolve is the first component's row
    kd.ctx      = this;
    kd.resolve  = table_resolve;
    kd.describe = table_describe;
    kd.name_of  = table_name_of;
    kd.find_name = table_find_name;
    kd.flags    = KV_HANDLE_GENERATIONAL;
    kind = handle_kind_register(kd);
    if (!kind) return false;
    slots.use_kind(kind);
    return slots.init(capacity, t);
}

void Table::free() {
    if (kind) kv_handle_kind_unregister(kind);
    for (Column& c : columns) {
        usize bytes = os_page_align((usize)capacity * c.stride);
        usize committed = os_page_align((usize)committed_rows * c.stride);
        if (c.data) { mem_account_decommit(tag, committed); os_release(c.data, bytes); }
        if (c.prev) { mem_account_decommit(tag, committed); os_release(c.prev, bytes); }
    }
    columns.free();
    names.free();
    for (Attachment& a : attachments) a.rows.free();
    attachments.free();
    props.free();
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
        if (committed_rows + grow > capacity) grow = capacity - committed_rows;
        if (grow == 0) return false;
        u32 to = committed_rows + grow;
        usize total = 0, want = 0;
        for (Column& c : columns) {
            usize have = os_page_align((usize)committed_rows * c.stride);
            usize need = os_page_align((usize)to * c.stride);
            if (need > have) want += (need - have) * (c.prev ? 2 : 1);
        }
        if (!mem_heap_admit(tag, want)) return false; // the hard limit said so, with the message
        for (Column& c : columns) {
            usize have = os_page_align((usize)committed_rows * c.stride);
            usize need = os_page_align((usize)to * c.stride);
            if (need <= have) continue;
            if (!os_commit(c.data + have, need - have)) { KV_ERROR("kind", "'%s': commit failed", name); return false; }
            total += need - have;
            if (c.prev) {
                if (!os_commit(c.prev + have, need - have)) { KV_ERROR("kind", "'%s': commit failed", name); return false; }
                total += need - have;
            }
        }
        committed_rows = to;
        mem_account_commit(tag, total);
    }
    return true;
}

Handle Table::spawn() {
    slots.owner = thread_id(); // the schedule made this thread the writer for now
    slots.tag   = tag;
    if (slots.live >= capacity) {
        // Logged once: a kind that fills under load would otherwise log every spawn it refuses.
        KV_MARK("kind full");
        if (refused++ == 0) KV_ERROR("kind", "'%s' is full (%u entities); refusals are counted from here", name, capacity);
        return Handle{};
    }
    // The rows for the slot about to be handed out must exist first: a slot taken and
    // given back on a failed commit would sit at the head of the free list, rows still
    // uncommitted, and fail every spawn after it.
    u32 next = slots.free_head != KV_POOL_NONE ? slots.free_head : slots.len;
    if (next >= committed_rows && !ensure_rows(next + 1)) return Handle{};
    Handle h = slots.create(0);
    if (h.is_null()) return h;
    ++churn;
    u32 i = h.index();
    for (Column& c : columns) {
        memset(row(c, i), 0, c.stride);
        if (c.prev) memset(c.prev + (usize)i * c.stride, 0, c.stride);
    }
    return h;
}

bool Table::destroy(Handle h) {
    slots.owner = thread_id();
    if (!slots.destroy(h)) return false;
    ++churn;
    if (names.count() || props.len) sparse_dirty = true;
    for (const Attachment& a : attachments) if (a.rows.count()) sparse_dirty = true;
#if KV_DEV
    if (!dev_spike_hunting()) {
        for (Column& c : columns) {
            memset(row(c, h.index()), 0xDD, c.stride); // a stale pointer sees poison
            if (c.prev) memset(c.prev + (usize)h.index() * c.stride, 0xDD, c.stride);
        }
    }
#endif
    return true;
}

bool Table::restore_slots(Span<const u32> gen_words, Span<const u32> free_order) {
    slots.owner = thread_id();
    slots.tag   = tag;
    destroy_all();
    if (gen_words.len > capacity || !ensure_rows(gen_words.len)) {
        KV_ERROR("kind", "'%s': cannot restore %u slots (capacity %u)", name, gen_words.len, capacity);
        slots.restore_slots(Span<const u32>(), Span<const u32>());
        return false;
    }
    for (Column& c : columns) {
        memset(c.data, 0, (usize)gen_words.len * c.stride);
        if (c.prev) memset(c.prev, 0, (usize)gen_words.len * c.stride);
    }
    names.clear(); // the old names go: the engine's map finds them no longer held
    for (Attachment& a : attachments) a.rows.clear();
    props.clear();
    ++churn;
    ++restores;
    return slots.restore_slots(gen_words, free_order);
}

u32 Table::destroy_all() {
    u32 n = rows(), gone = 0;
    for (u32 i = 0; i < n; ++i)
        if (is_alive(i) && destroy(handle_at(i))) ++gone;
    return gone;
}

void Table::begin_tick() {
    // A still kind's rows equal their previous copies unless it spawned since the last copy.
    if (desc && desc->still) {
        if (churn == churn_copied) return;
        churn_copied = churn;
    }
    u32 n = rows();
    for (Column& c : columns)
        if (c.prev) memcpy(c.prev, c.data, (usize)n * c.stride);
}

u64 Table::hash() const {
    u64 h = hash_str(name);
    u32 n = rows();
    for (u32 i = 0; i < n; ++i) {
        if (!is_alive(i)) continue;
        // Generation and index only: the handle kind is a process-wide number that depends
        // on what else registered, and the table's name already stands for the kind.
        h = hash_mix(h ^ (handle_at(i).bits & 0x000FFFFFFFFFFFFFull));
        for (const Column& c : columns)
            if (c.desc->is_sim()) h = hash_mix(h ^ component_hash(*c.desc, row(c, i)));
    }
    // Names, attachments and bags are the entity's: a kind with none hashes as it always did.
    for (u32 i = 0; i < names.count(); ++i)
        if (sparse_live(names, i)) h = hash_mix(h ^ ((u64)names.slots[i] * 0x9E3779B97F4A7C15ull) ^ ((const Name*)names.at(i))->hash);
    for (const Attachment& a : attachments) {
        if (!a.desc->is_sim()) continue;
        u64 ah = hash_str(a.desc->name);
        for (u32 i = 0; i < a.rows.count(); ++i)
            if (sparse_live(a.rows, i)) h = hash_mix(h ^ ah ^ ((u64)a.rows.slots[i] << 20) ^ component_hash(*a.desc, a.rows.at(i)));
    }
    for (const PropEntry& p : props) {
        if (!is_alive(p.slot) || handle_at(p.slot).gen() != p.gen) continue;
        // A handle's kind is a number that depends on what registered first; its name is what
        // the kind is, as for a column's handle fields.
        u64 bits = p.value.bits;
        if (p.value.type == PropType::Handle && bits)
            bits = (bits & 0x000FFFFFFFFFFFFFull) ^ kv_handle_kind_name_hash(Handle{bits}.kind());
        u64 v = hash_mix(p.key.hash ^ (u64)p.value.type ^ (u64)p.value.i * 31 ^ bits * 131);
        h = hash_mix(h ^ ((u64)p.slot << 24) ^ v ^ hash_bytes(&p.value.f, sizeof p.value.f));
    }
    return h;
}

Name Table::name_of(Handle h) const {
    if (!valid(h)) return Name{};
    u32 i = names.find(h.index());
    return i != KV_POOL_NONE && names.gens[i] == h.gen() ? *(const Name*)names.at(i) : Name{};
}

void Table::sweep_sparse(Array<Name>* gone) {
    sparse_dirty = false;
    for (u32 i = names.count(); i-- > 0;) {
        if (sparse_live(names, i)) continue;
        if (gone) gone->push(*(const Name*)names.at(i));
        names.erase(i);
    }
    for (Attachment& a : attachments)
        for (u32 i = a.rows.count(); i-- > 0;)
            if (!sparse_live(a.rows, i)) a.rows.erase(i);
    u32 kept = 0;
    for (u32 i = 0; i < props.len; ++i)
        if (is_alive(props[i].slot) && handle_at(props[i].slot).gen() == props[i].gen) props[kept++] = props[i];
    props.len = kept;
}

// ---- attachments ------------------------------------------------------------------------------

Attachment* Table::attachment(u32 component_id) {
    for (Attachment& a : attachments)
        if (*a.desc->id == component_id) return &a;
    return nullptr;
}

void* Table::attached(Handle h, u32 component_id) {
    Attachment* a = attachment(component_id);
    if (!a || !valid(h)) return nullptr;
    u32 i = a->rows.find(h.index());
    return i != KV_POOL_NONE && a->rows.gens[i] == h.gen() ? a->rows.at(i) : nullptr;
}

void* Table::attach(Handle h, u32 component_id, const void* data) {
    Attachment* a = attachment(component_id);
    if (!a || !valid(h)) return nullptr;
    u8* row = a->rows.insert(h.index(), h.gen());
    memcpy(row, data, a->desc->size);
    return row;
}

bool Table::detach(Handle h, u32 component_id) {
    Attachment* a = attachment(component_id);
    if (!a || !valid(h)) return false;
    u32 i = a->rows.find(h.index());
    if (i == KV_POOL_NONE || a->rows.gens[i] != h.gen()) return false;
    a->rows.erase(i);
    return true;
}

// ---- property bags ----------------------------------------------------------------------------

namespace {

// Where (slot, key) is or would go in the sorted entries.
u32 prop_lower(const Array<PropEntry>& ps, u32 slot, u64 key) {
    u32 lo = 0, hi = ps.len;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        const PropEntry& p = ps.data[mid];
        if (p.slot < slot || (p.slot == slot && p.key.hash < key)) lo = mid + 1; else hi = mid;
    }
    return lo;
}

} // namespace

const PropValue* Table::prop(Handle h, Name key) const {
    if (!desc->props || !valid(h)) return nullptr;
    u32 i = prop_lower(props, h.index(), key.hash);
    if (i < props.len && props[i].slot == h.index() && props[i].key == key && props[i].gen == h.gen()) return &props[i].value;
    return nullptr;
}

bool Table::prop_set(Handle h, Name key, const PropValue& v) {
    if (!desc->props || !valid(h) || key.is_null()) return false;
    // A stale bag left in the slot by a dead entity goes first.
    u32 at = prop_lower(props, h.index(), 0), kept = at;
    u32 end = at;
    while (end < props.len && props[end].slot == h.index()) ++end;
    for (u32 k = at; k < end; ++k) if (props[k].gen == h.gen()) props[kept++] = props[k];
    if (kept != end) {
        memmove(props.data + kept, props.data + end, (usize)(props.len - end) * sizeof(PropEntry));
        props.len -= end - kept;
    }
    u32 i = prop_lower(props, h.index(), key.hash);
    if (i < props.len && props[i].slot == h.index() && props[i].key == key) {
        props[i].gen = h.gen();
        props[i].value = v;
        return true;
    }
    props.push(PropEntry{});
    memmove(props.data + i + 1, props.data + i, (usize)(props.len - 1 - i) * sizeof(PropEntry));
    props[i] = PropEntry{h.index(), h.gen(), key, v};
    return true;
}

bool Table::prop_remove(Handle h, Name key) {
    if (!desc->props || !valid(h)) return false;
    u32 i = prop_lower(props, h.index(), key.hash);
    if (i >= props.len || props[i].slot != h.index() || props[i].key != key || props[i].gen != h.gen()) return false;
    memmove(props.data + i, props.data + i + 1, (usize)(props.len - 1 - i) * sizeof(PropEntry));
    --props.len;
    return true;
}

u32 Table::props_of(Handle h, u32* first) const {
    u32 i = prop_lower(props, h.index(), 0), n = 0;
    *first = i;
    while (i + n < props.len && props[i + n].slot == h.index()) ++n;
    if (n && props[i].gen != h.gen()) return 0; // a dead entity's, until the sweep (a slot's entries share one generation)
    return n;
}

int Table::describe(Handle h, char* out, usize cap) const {
    if (!valid(h))
        return format(out, cap, "%s (%u live)", h.index() < rows() ? "stale" : "out of range", live());
    usize at = 0;
    // A separator goes in only with room for a terminator after it, and counts either way,
    // as snprintf's length does. (Written into the last byte, it left the text unterminated
    // whenever the component before it ended one byte short of the buffer.)
    auto sep = [&](char ch) {
        if (at + 1 < cap) { out[at] = ch; out[at + 1] = 0; }
        ++at;
    };
    for (u32 k = 0; k < columns.len; ++k) {
        const Column& c = columns[k];
        if (k) sep(' ');
        int n = component_describe(*c.desc, row(c, h.index()), out + (at < cap ? at : cap), at < cap ? cap - at : 0);
        if (n > 0) at += (usize)n;
    }
    for (const Attachment& a : attachments) {
        const void* r = attached(h, *a.desc->id);
        if (!r) continue;
        sep(' ');
        sep('@');
        int n = component_describe(*a.desc, r, out + (at < cap ? at : cap), at < cap ? cap - at : 0);
        if (n > 0) at += (usize)n;
    }
    u32 first = 0, np = desc->props ? props_of(h, &first) : 0;
    for (u32 k = 0; k < np; ++k) {
        const PropEntry& p = props[first + k];
        int n = 0;
        char* dst = out + (at < cap ? at : cap);
        usize room = at < cap ? cap - at : 0;
        const char* lead = k ? ", " : " @props{";
        switch (p.value.type) {
        case PropType::Int:    n = format(dst, room, "%s%s=%lld", lead, name_str(p.key), (long long)p.value.i); break;
        case PropType::Float:  n = format(dst, room, "%s%s=%g", lead, name_str(p.key), p.value.f); break;
        case PropType::Bool:   n = format(dst, room, "%s%s=%s", lead, name_str(p.key), p.value.i ? "true" : "false"); break;
        case PropType::String: n = format(dst, room, "%s%s=\"%s\"", lead, name_str(p.key), name_str(Name{p.value.bits})); break;
        case PropType::Handle: n = format(dst, room, "%s%s=%s", lead, name_str(p.key), kind_name(Handle{p.value.bits})); break;
        case PropType::None:   break;
        }
        if (n > 0) at += (usize)n;
        if (k + 1 == np && at + 1 < cap) { out[at++] = '}'; out[at] = 0; }
    }
    return (int)at;
}

} // namespace kv
