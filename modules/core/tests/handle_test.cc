#include "korvid/core/pool.h"
#include "korvid/core/test.h"
#include <string.h>

struct Thing { kv::u32 value; };
struct Other { kv::f32 x; };

// An owner registering a pool as a kind, with the pool's own meaning of resolve.
static kv::u16 register_pool(kv::Pool<Thing>& pool, const char* name) {
    kv_handle_kind_desc desc{};
    desc.name     = name;
    desc.type_key = kv::type_key<Thing>();
    desc.ctx      = &pool;
    desc.resolve  = kv::Pool<Thing>::resolve_fn;
    desc.describe = kv::Pool<Thing>::describe_fn;
    desc.flags    = KV_HANDLE_GENERATIONAL;
    kv::u16 kind = kv::handle_kind_register(desc);
    pool.use_kind(kind);
    return kind;
}

void test_handle(kv::TestCtx& c) {
    using namespace kv;
    c.begin("handle");

    // A pool with no owner-registered kind issues local handles: kind 0, nothing to the registry.
    Pool<Thing> local;
    Handle l = local.create(Thing{9});
    KV_CHECK(c, l.kind() == 0 && local.get(l)->value == 9 && resolve(l) == nullptr);
    local.free();

    Pool<Thing> pool;
    {
        char name[16];
        memcpy(name, "test.thing", 11);
        KV_CHECK(c, register_pool(pool, name) != 0);
        memset(name, 'x', sizeof name); // the registry copied the descriptor and the name
    }
    KV_CHECK(c, pool.kind != 0 && strcmp(kv_handle_kind_name(pool.kind), "test.thing") == 0);
    const kv_handle_kind_desc* reg = kv_handle_kind_desc_get(pool.kind);
    KV_CHECK(c, reg && reg->ctx == &pool && reg->type_key == type_key<Thing>() && strcmp(reg->name, "test.thing") == 0);
    KV_CHECK(c, kv_handle_kind_desc_get(0) == nullptr);
    Handle a = pool.create(Thing{1});
    KV_CHECK(c, a.kind() == pool.kind && a.index() == 0 && a.gen() == 1);
    Handle b = pool.create(Thing{2});
    Handle d = pool.create(Thing{3});
    KV_CHECK(c, pool.live == 3);
    KV_CHECK(c, pool.get(a) && pool.get(a)->value == 1);
    KV_CHECK(c, pool.get(b) && pool.get(b)->value == 2);
    KV_CHECK(c, pool.get(d) && pool.get(d)->value == 3);

    // Null handle is never valid.
    KV_CHECK(c, !pool.valid(Handle{}));
    KV_CHECK(c, pool.get(Handle{}) == nullptr);

    // Destroy, then use the old handle: an error, never another entity's data. In dev
    // builds the slot is poisoned, so a pointer kept across the destroy reads 0xDD.
    Thing* b_ptr = pool.get(b);
    KV_CHECK(c, pool.destroy(b));
#if KV_DEV
    KV_CHECK(c, b_ptr->value == 0xDDDDDDDDu);
#else
    KV_UNUSED(b_ptr);
#endif
    KV_CHECK(c, pool.live == 2);
    KV_CHECK(c, pool.get(b) == nullptr);
    KV_CHECK(c, !pool.destroy(b)); // double destroy is reported, not fatal

    // The slot is reused with a new generation; the stale handle stays stale.
    Handle e = pool.create(Thing{4});
    KV_CHECK(c, e.index() == b.index());
    KV_CHECK(c, e.gen() != b.gen());
    KV_CHECK(c, pool.get(e) && pool.get(e)->value == 4);
    KV_CHECK(c, pool.get(b) == nullptr);

    // Out-of-range index with a plausible generation is also rejected.
    KV_CHECK(c, pool.get(Handle::make(pool.kind, 1, 99)) == nullptr);

    // Free slots come back FIFO: destroying two, the first destroyed is reused first.
    pool.destroy(e);
    pool.destroy(d);
    Handle r1 = pool.create(Thing{5});
    Handle r2 = pool.create(Thing{6});
    KV_CHECK(c, r1.index() == e.index() && r2.index() == d.index());

    // A handle from another pool fails the kind check, even with a plausible slot.
    Pool<Thing> twin;
    KV_CHECK(c, register_pool(twin, "test.twin") != 0);
    Handle t = twin.create(Thing{77});
    KV_CHECK(c, t.index() == a.index() && t.kind() != a.kind());
    KV_CHECK(c, pool.get(t) == nullptr && twin.get(a) == nullptr);
    KV_CHECK(c, twin.get(t)->value == 77);

    // Blind resolution: whoever issued it answers, and the type key is checked on request.
    KV_CHECK(c, resolve(a) == pool.get(a) && resolve(t) == twin.get(t));
    KV_CHECK(c, resolve<Thing>(a) == pool.get(a));
    KV_CHECK(c, resolve<Other>(a) == nullptr);
    KV_CHECK(c, resolve(b) == nullptr && resolve(Handle{}) == nullptr);
    KV_CHECK(c, kv_handle_type_key(a.bits) == type_key<Thing>());
    TypedHandle<Thing> ta(a);
    KV_CHECK(c, ta.get() == pool.get(a) && Handle(ta) == a);
    KV_CHECK(c, strcmp(kind_name(a), "test.thing") == 0 && kind_name(Handle{})[0] == 0);

    // Described for humans, including the issuer's own text.
    char text[128];
    KV_CHECK(c, kv_handle_describe(a.bits, text, sizeof text) > 0);
    KV_CHECK(c, strncmp(text, "test.thing#0:1 alive", 20) == 0);
    kv_handle_describe(b.bits, text, sizeof text);
    KV_CHECK(c, strstr(text, "stale") != nullptr);
    kv_handle_describe(0, text, sizeof text);
    KV_CHECK(c, strcmp(text, "null") == 0);
    KV_CHECK(c, kv_handle_describe(a.bits, text, 8) == 7 && strlen(text) == 7); // truncates cleanly

    // A pool may be moved by value; the owner points the kind at the new location.
    Pool<Thing> moved = twin;
    twin = Pool<Thing>{};
    kv_handle_kind_set_ctx(moved.kind, &moved);
    KV_CHECK(c, resolve(t) == moved.get(t) && moved.get(t)->value == 77);
    KV_CHECK(c, kv_handle_kind_desc_get(moved.kind)->ctx == &moved);
    twin = moved;
    kv_handle_kind_set_ctx(twin.kind, &twin);

    // Unregistering the kind makes every handle it issued resolve to null; ids are not reused.
    u16 twin_kind = twin.kind;
    kv_handle_kind_unregister(twin_kind);
    twin.free();
    KV_CHECK(c, resolve(t) == nullptr && !kv_handle_kind_is_live(twin_kind));
    Pool<Thing> again;
    KV_CHECK(c, register_pool(again, "test.again") != twin_kind);
    kv_handle_kind_unregister(again.kind);
    again.free();

    // Addresses are stable across growth: the pool commits pages into one reservation.
    Thing* first = pool.get(a);
    for (u32 i = 0; i < 5000; ++i) pool.create(Thing{i});
    KV_CHECK(c, pool.get(a) == first && first->value == 1);
    KV_CHECK(c, pool.committed >= pool.len && pool.len == 5003);
    KV_CHECK(c, pool.handle_at(a.index()) == a);

    // Snapshot, mutate, restore: handles and staleness come back exactly.
    Handle f = pool.create(Thing{42});
    usize size = pool.snapshot_size();
    u8* shot = (u8*)mem_alloc(size);
    pool.snapshot(shot);
    pool.destroy(f);
    pool.get(a)->value = 1000;
    Handle g = pool.create(Thing{7}); // reuses f's slot with a new generation
    KV_CHECK(c, g.index() == f.index() && pool.get(f) == nullptr);
    KV_CHECK(c, pool.restore(shot));
    KV_CHECK(c, pool.get(f) && pool.get(f)->value == 42);
    KV_CHECK(c, pool.get(g) == nullptr);           // never existed in the snapshot
    KV_CHECK(c, pool.get(a)->value == 1);          // the mutation is undone
    KV_CHECK(c, pool.get(b) == nullptr);           // still stale
    KV_CHECK(c, pool.live == 5004);
    mem_free(shot);

    // Restoring into a fresh pool works too (the snapshot is relocatable). The copy is
    // its own kind, so the original's handles are foreign to it; its own slots match.
    Pool<Thing> other;
    shot = (u8*)mem_alloc(size);
    pool.snapshot(shot);
    KV_CHECK(c, other.restore(shot));
    KV_CHECK(c, other.get(f) == nullptr && other.kind != pool.kind);
    Handle f2 = other.handle_at(f.index());
    KV_CHECK(c, other.get(f2) && other.get(f2)->value == 42 && other.live == pool.live);
    mem_free(shot);
    other.free();

    kv_handle_kind_unregister(pool.kind);
    pool.free();
    KV_CHECK(c, pool.live == 0 && pool.base == nullptr);
}
