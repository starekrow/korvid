// The references (floor.h): a pool with nothing in it, and the engine scenario with no
// engine under it.
#include "floor.h"
#include "korvid/core/atomic.h"
#include "korvid/core/hash.h"
#include "korvid/core/memory.h"
#include "korvid/core/os_thread.h"

namespace kv {

namespace {

// One batch at a time, in place: there is no queue to put a second one in.
//
// `gen` is odd while a batch is open. A worker announces itself in `inside` before it
// reads `gen`, and the caller closes `gen` before it reads `inside`, both sequentially
// consistent, so one of the two always sees the other: when the caller has seen `inside`
// at zero after closing, no worker is in the batch or can get in, and the fields are the
// caller's to write again.
struct Floor {
    JobFn     fn = nullptr;
    void*     user = nullptr;
    u32       count = 0, grain = 0, chunks = 0;
    // Each shared word on a cache line of its own (padded by hand: cl warns of alignas here).
    u8        pad0[64] = {};
    u32       next = 0;   u8 pad1[60] = {}; // atomic: the next chunk to claim
    u32       done = 0;   u8 pad2[60] = {}; // atomic: chunks finished
    u32       gen = 0;    u8 pad3[60] = {}; // atomic: odd while a batch is open
    u32       inside = 0; u8 pad4[60] = {}; // atomic: workers between announcing and leaving
    u32       quit = 0;   u8 pad5[60] = {}; // atomic
    OsThread* threads = nullptr;
    u32       workers = 0;
};
alignas(64) constinit Floor g_floor;

void work(Floor& f) {
    for (;;) {
        u32 c = atomic_fetch_add(&f.next, 1, MO_RELAXED);
        if (c >= f.chunks) return;
        u32 begin = c * f.grain;
        u32 end   = begin + f.grain < f.count ? begin + f.grain : f.count;
        f.fn(c, begin, end, f.user);
        atomic_add_fetch(&f.done, 1, MO_RELEASE);
    }
}

void floor_worker(void*) {
    Floor& f = g_floor;
    u32 seen = 0;
    while (!atomic_load(&f.quit, MO_RELAXED)) {
        if (atomic_load(&f.gen, MO_RELAXED) == seen) { atomic_pause(); continue; }
        atomic_add_fetch(&f.inside, 1, MO_SEQ_CST);
        u32 g = atomic_load(&f.gen, MO_SEQ_CST);
        if (g & 1) work(f);
        seen = g;
        atomic_sub_fetch(&f.inside, 1, MO_SEQ_CST);
    }
}

} // namespace

bool floor_init(u32 workers) {
    Floor& f = g_floor;
    floor_shutdown();
    atomic_store(&f.quit, 0, MO_RELAXED);
    if (!workers) return true;
    f.threads = alloc_items<OsThread>(mem_general(), workers);
    if (!f.threads) return false;
    for (u32 i = 0; i < workers; ++i) {
        f.threads[i] = OsThread();
        if (!os_thread_start(&f.threads[i], floor_worker, nullptr)) { floor_shutdown(); return false; }
        ++f.workers;
    }
    return true;
}

void floor_shutdown() {
    Floor& f = g_floor;
    if (!f.threads) return;
    atomic_store(&f.quit, 1, MO_RELAXED);
    for (u32 i = 0; i < f.workers; ++i) os_thread_join(&f.threads[i]);
    free_items(mem_general(), f.threads, f.workers ? f.workers : 1);
    f.threads = nullptr;
    f.workers = 0;
}

void floor_run(u32 count, u32 grain, JobFn fn, void* user) {
    Floor& f = g_floor;
    if (!grain) return;
    u32 chunks = count / grain + (count % grain != 0);
    if (chunks == 0) return;
    f.fn = fn; f.user = user; f.count = count; f.grain = grain; f.chunks = chunks;
    atomic_store(&f.next, 0, MO_RELAXED);
    atomic_store(&f.done, 0, MO_RELAXED);
    atomic_add_fetch(&f.gen, 1, MO_SEQ_CST); // open
    work(f);
    while (atomic_load(&f.done, MO_ACQUIRE) != chunks) atomic_pause();
    atomic_add_fetch(&f.gen, 1, MO_SEQ_CST); // closed
    while (atomic_load(&f.inside, MO_SEQ_CST) != 0) atomic_pause();
}

// ---- the plain sim ---------------------------------------------------------------------

struct PlainSim {
    u32  bodies;
    struct Pos { f32 x, y; }* pos; // the layout stress.cc has: a column a component
    struct Vel { f32 dx, dy; }* vel;
    u32* age;
    u8*  alive;
    u64  tick;
    u64  bounces; // atomic
    f64  energy;
    f64* sums;    // a chunk's energy each, folded in chunk order
};

namespace {

constexpr f32 kPlainBox = 1000.0f;

void run(PlainSim* s, bool parallel, u32 grain, JobFn fn) {
    if (parallel) { floor_run(s->bodies, grain, fn, s); return; }
    u32 chunks = s->bodies / grain + (s->bodies % grain != 0); // grain is never 0 here
    for (u32 c = 0; c < chunks; ++c) fn(c, c * grain, (c + 1) * grain < s->bodies ? (c + 1) * grain : s->bodies, s);
}

void plain_bounce(u32, u32 b, u32 e, void* user) {
    PlainSim& s = *(PlainSim*)user;
    u64 hits = 0;
    for (u32 i = b; i < e; ++i) {
        if (!s.alive[i]) continue;
        bool hit = false;
        if (s.pos[i].x < -kPlainBox) { s.pos[i].x = -kPlainBox; s.vel[i].dx = -s.vel[i].dx; hit = true; }
        if (s.pos[i].x >  kPlainBox) { s.pos[i].x =  kPlainBox; s.vel[i].dx = -s.vel[i].dx; hit = true; }
        if (s.pos[i].y < -kPlainBox) { s.pos[i].y = -kPlainBox; s.vel[i].dy = -s.vel[i].dy; hit = true; }
        if (s.pos[i].y >  kPlainBox) { s.pos[i].y =  kPlainBox; s.vel[i].dy = -s.vel[i].dy; hit = true; }
        hits += hit;
    }
    atomic_add_fetch(&s.bounces, hits, MO_RELAXED);
}

void plain_integrate(u32, u32 b, u32 e, void* user) {
    PlainSim& s = *(PlainSim*)user;
    for (u32 i = b; i < e; ++i) {
        if (!s.alive[i]) continue;
        s.pos[i].x += s.vel[i].dx * (1.0f / 60.0f);
        s.pos[i].y += s.vel[i].dy * (1.0f / 60.0f);
    }
}

// The engine's churn destroys a row and spawns another; here the row is rewritten where it
// lies, which is the cheapest thing that could be meant by it.
void plain_churn(u32, u32 b, u32 e, void* user) {
    PlainSim& s = *(PlainSim*)user;
    for (u32 i = b; i < e; ++i) {
        if (!s.alive[i]) continue;
        if (++s.age[i] % 600 == (s.tick % 600)) {
            s.pos[i].x = -s.pos[i].x; s.pos[i].y = -s.pos[i].y;
            s.vel[i].dx = (f32)((i * 7) % 200) - 100.0f; s.vel[i].dy = (f32)((i * 13) % 200) - 100.0f;
            s.age[i] = 0;
        }
    }
}

void plain_energy(u32 chunk, u32 b, u32 e, void* user) {
    PlainSim& s = *(PlainSim*)user;
    f64 sum = 0.0;
    for (u32 i = b; i < e; ++i) if (s.alive[i]) sum += (f64)(s.vel[i].dx * s.vel[i].dx + s.vel[i].dy * s.vel[i].dy);
    s.sums[chunk] = sum;
}

// A float in [lo, hi) from a counter: the values only have to fill the box.
f32 spread(u64* state, f32 lo, f32 hi) {
    *state = hash_mix(*state + 0x9e3779b97f4a7c15ull);
    return lo + (hi - lo) * (f32)(*state >> 40) * (1.0f / 16777216.0f);
}

} // namespace

PlainSim* plain_create(u32 bodies) {
    Allocator* a = mem_general();
    PlainSim* s = alloc_items<PlainSim>(a, 1);
    if (!s) return nullptr;
    *s = PlainSim{};
    s->bodies = bodies;
    s->pos = alloc_items<PlainSim::Pos>(a, bodies);
    s->vel = alloc_items<PlainSim::Vel>(a, bodies);
    s->age = alloc_items<u32>(a, bodies);
    s->alive = alloc_items<u8>(a, bodies);
    s->sums = alloc_items<f64>(a, jobs_chunks(bodies, 4096));
    if (!s->pos || !s->vel || !s->age || !s->alive || !s->sums) { plain_destroy(s); return nullptr; }
    u64 r = 99;
    for (u32 i = 0; i < bodies; ++i) {
        s->pos[i].x = spread(&r, -kPlainBox, kPlainBox); s->pos[i].y = spread(&r, -kPlainBox, kPlainBox);
        s->vel[i].dx = spread(&r, -300, 300);  s->vel[i].dy = spread(&r, -300, 300);
        s->age[i] = (u32)(hash_mix(i) % 600);
        s->alive[i] = 1;
    }
    return s;
}

void plain_destroy(PlainSim* s) {
    if (!s) return;
    Allocator* a = mem_general();
    u32 n = s->bodies;
    if (s->pos) free_items(a, s->pos, n);
    if (s->vel) free_items(a, s->vel, n);
    if (s->age) free_items(a, s->age, n);
    if (s->alive) free_items(a, s->alive, n);
    if (s->sums) free_items(a, s->sums, jobs_chunks(n, 4096));
    free_items(a, s, 1);
}

void plain_tick(PlainSim* s, bool parallel) {
    run(s, parallel, 2048, plain_bounce);
    run(s, parallel, 2048, plain_integrate);
    run(s, parallel, 4096, plain_churn);
    run(s, parallel, 4096, plain_energy);
    f64 sum = 0.0;
    for (u32 c = 0, n = jobs_chunks(s->bodies, 4096); c < n; ++c) sum += s->sums[c];
    s->energy = sum;
    ++s->tick;
}

u64 plain_bounces(const PlainSim* s) { return atomic_load(&s->bounces, MO_RELAXED); }

} // namespace kv
