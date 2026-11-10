// example_arcfire: patterns. Every enemy fires a BulletML pattern from the library
// (assets/patterns/: patterns.json names the documents, each one pattern), compiled once
// at load by the bulletml module. An enemy's Runner runs its pattern; a bullet whose
// definition has actions of its own is an `active` bullet with a Runner too. The
// `patterns` system steps every runner of every kind in a parallel_for: a fire spawns by
// the chunk's commands, so bullets arrive in chunk order whatever ran where, and every
// runner's randomness is its own stream, seeded from its entity and the tick.
#include "arcfire.h"
#include "korvid/asset/blob.h"
#include "korvid/core/hash.h"
#include "korvid/core/json.h"
#include "korvid/core/log.h"
#include <stdio.h>
#include <string.h>

namespace kv::arcfire {

u64 seed_of(Handle h, u64 tick, u32 salt) { return hash_mix(h.bits ^ hash_mix(tick * 0x9E3779B97F4A7C15ull + salt)); }

bool patterns_load(Engine& e, Game& g) {
    g.pattern_index = asset_load_blob(e, "example_arcfire/patterns/patterns.json");
    return !g.pattern_index.is_null();
}

// The index first, then its documents, then the library: true once it is built. A file
// that does not load or compile is `failed`, with the reason logged.
bool patterns_ready(Engine& e, Game& g, bool& failed) {
    failed = false;
    if (g.lib.entries.len) return true;
    const BlobAsset* index = asset_blob(e, g.pattern_index);
    if (!index || index->state == AssetState::Pending) return false;
    if (index->state != AssetState::Ready) { KV_ERROR("arcfire", "patterns.json did not load"); failed = true; return false; }
    if (!g.pattern_count) {
        MemScope scope;
        JsonDoc doc;
        if (!json_parse((const char*)index->data, index->size, mem_scratch(), doc) || !doc.root().is_array()) {
            KV_ERROR("arcfire", "patterns.json: %s", doc.error ? doc.error : "not a list of names");
            failed = true;
            return false;
        }
        for (const JsonValue* v = json_first(doc, doc.root()); v && g.pattern_count < Game::kMaxPatterns; v = json_next(doc, *v)) {
            char path[128];
            snprintf(g.pattern_names[g.pattern_count], kPatternName, "%s", json_string(v, ""));
            snprintf(path, sizeof path, "example_arcfire/patterns/%s.xml", g.pattern_names[g.pattern_count]);
            g.pattern_blobs[g.pattern_count++] = asset_load_blob(e, path);
        }
    }
    for (u32 i = 0; i < g.pattern_count; ++i) {
        const BlobAsset* b = asset_blob(e, g.pattern_blobs[i]);
        if (!b || b->state == AssetState::Pending) return false;
        if (b->state != AssetState::Ready) { KV_ERROR("arcfire", "pattern %s did not load", g.pattern_names[i]); failed = true; return false; }
    }
    for (u32 i = 0; i < g.pattern_count; ++i) {
        const BlobAsset* b = asset_blob(e, g.pattern_blobs[i]);
        char why[256];
        if (bml_compile(g.lib, g.pattern_names[i], (const char*)b->data, b->size, why, sizeof why) == KV_BML_NONE) {
            KV_ERROR("arcfire", "pattern %s: %s", g.pattern_names[i], why);
            failed = true;
            bml_free(g.lib);
            return false;
        }
    }
    // A bullet's look, by its definition's label: needles are needles, the rest are round.
    for (u32 b = 0; b < g.lib.bullets.len && b < sizeof g.bullet_look; ++b)
        g.bullet_look[b] = !strncmp(bml_bullet_label(g.lib, b), "needle", 6) ? 1 : 0;
    KV_INFO("arcfire", "patterns: %u compiled, %u actions, %u ops, %u bullet definitions", g.lib.entries.len, g.lib.actions.len, g.lib.ops.len,
            g.lib.bullets.len);
    return true;
}

void emitter_start(const Game& g, Emitter& em, Runner& r, Handle owner, u64 tick) {
    bml_start(g.lib, r, em.pattern, seed_of(owner, tick, em.pattern));
    em.state = EmitRunning;
}

namespace {

// What a chunk's fires need: where the runner is, who it is, and the chunk to spawn by.
struct FireCtx {
    ChunkContext* cc;
    const Game*   g;
    const Dials*  d;
    point2        at;
    Handle        owner;
    u64           tick;
    u32           fired;
};

void on_fire(void* user, const BmlFire& f) {
    FireCtx& c = *(FireCtx*)user;
    bullet_fire(*c.cc, *c.g, *c.d, c.at, f, c.owner, seed_of(c.owner, c.tick, 0x100 + c.fired));
    ++c.fired;
}

} // namespace

// Phase Sim, after the enemies and the bosses (which start their emitters) and after
// collision.compose (a boss's parts fire from where they are this tick): every enemy's
// runner steps, and its pattern fires.
void patterns(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    Span<const Transform2> ship_xf = ctx.read<Transform2>("example_arcfire.ship.Transform2");
    Span<const Dials> dials_row = ctx.read<Dials>(ARC_WORLD(Dials));
    if (!dials_row.len || g.ship.index() >= ship_xf.len || !g.lib.entries.len) return;
    const Dials& d = dials_row[0];
    const point2 target = ship_xf[g.ship.index()].position;
    const Game& cg = g;
    const u64 tick = ctx.tick;
    struct Counts { u32 fires, stepped; };
    auto add = [](Counts a, Counts b) { return Counts{a.fires + b.fires, a.stepped + b.stepped}; };
    Counts total{0, 0};

    // The enemies' runners: a pattern runs while the emitter says so.
    for (KindRows k : ctx.view<const Transform2, Emitter, Runner>()) {
        Span<const Transform2> t = k.col<const Transform2>();
        Span<Emitter> em = k.col<Emitter>();
        Span<Runner> run = k.col<Runner>();
        total = add(total, parallel_reduce(ctx, k.rows(), 64, Counts{0, 0}, [&](ChunkContext& cc, u32 begin, u32 end) {
            Counts n{0, 0};
            for (u32 i = begin; i < end; ++i) {
                if (!k.alive(i) || em[i].state != EmitRunning) continue;
                FireCtx fc{&cc, &cg, &d, t[i].position, k.handle_at(i), tick, 0};
                BmlStep st = bml_step(cg.lib, run[i], BmlWorld{t[i].position, target, d.rank}, on_fire, &fc);
                if (st != BmlStep::Running) em[i].state = EmitDone;
                n.fires += fc.fired;
                ++n.stepped;
            }
            return n;
        }, add));
    }
    g.bullets_spawned = total.fires;
    g.runners_stepped = total.stepped;
}

// Phase Sim, before the bullets fly: the active bullets' programs steer them (their
// velocity follows), fire and vanish. Apart from `patterns`, which comes after every
// transform's writer (it reads the parts' composed places), as these write their own.
void actives(TickContext& ctx) {
    Game& g = ctx.state_as<Game>();
    Span<const Transform2> ship_xf = ctx.read<Transform2>("example_arcfire.ship.Transform2");
    Span<const Dials> dials_row = ctx.read<Dials>(ARC_WORLD(Dials));
    if (!dials_row.len || g.ship.index() >= ship_xf.len || !g.lib.entries.len) return;
    const Dials& d = dials_row[0];
    const point2 target = ship_xf[g.ship.index()].position;
    const Game& cg = g;
    const u64 tick = ctx.tick;
    struct Counts { u32 fires, stepped; };
    auto add = [](Counts a, Counts b) { return Counts{a.fires + b.fires, a.stepped + b.stepped}; };
    Counts total{0, 0};
    Table* actives = ctx.write_kind(g_active_kind);
    Span<Transform2> at = actives->rows_of<Transform2>();
    Span<Velocity> v = actives->rows_of<Velocity>();
    Span<Runner> run = actives->rows_of<Runner>();
    total = add(total, parallel_reduce(ctx, actives->rows(), 256, Counts{0, 0}, [&](ChunkContext& cc, u32 begin, u32 end) {
        Counts n{0, 0};
        for (u32 i = begin; i < end; ++i) {
            if (!actives->is_alive(i)) continue;
            Handle h = actives->handle_at(i);
            FireCtx fc{&cc, &cg, &d, at[i].position, h, tick, 0};
            Runner& r = run[i];
            if (bml_step(cg.lib, r, BmlWorld{at[i].position, target, d.rank}, on_fire, &fc) == BmlStep::Vanish) cc.destroy(h);
            v[i].v = bml_velocity(r.dir, r.speed, r.mx, r.my) * d.bullet_speed;
            at[i].rotation = -r.dir * (KV_PI / 180.0f);
            n.fires += fc.fired;
            ++n.stepped;
        }
        return n;
    }, add));
    g.bullets_spawned += total.fires;
    g.runners_stepped += total.stepped;
}

} // namespace kv::arcfire
