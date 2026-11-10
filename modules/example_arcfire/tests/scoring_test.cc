// The score's rules: the chain's steps, its window (a kill opens it, a graze holds it,
// a gap ends it), the extends' scores, an egg past five, the stage bonus.
#include "../src/scoring.h"
#include "korvid/core/test.h"

using namespace kv::arcfire;

void test_scoring(kv::TestCtx& c) {
    // The steps.
    KV_CHECK(c, chain_multiplier(0) == 1 && chain_multiplier(9) == 1 && chain_multiplier(10) == 2 && chain_multiplier(24) == 2);
    KV_CHECK(c, chain_multiplier(25) == 3 && chain_multiplier(49) == 3 && chain_multiplier(50) == 4 && chain_multiplier(99) == 4);
    KV_CHECK(c, chain_multiplier(100) == 5 && chain_multiplier(5000) == 5);

    // Ten kills in the window: the tenth pays double; a gap of the window ends the chain.
    Run r{};
    kv::i64 paid = 0;
    for (int i = 0; i < 9; ++i) { paid += chain_pay(r, 90, 100); for (int t = 0; t < 89; ++t) chain_tick(r); }
    KV_CHECK(c, r.chain == 9 && paid == 900 && r.chain_left == 1);
    KV_CHECK(c, chain_pay(r, 90, 100) == 200 && r.chain == 10 && r.chain_best == 10);
    bool ended = false;
    for (int t = 0; t < 90; ++t) ended = chain_tick(r) || ended;
    KV_CHECK(c, ended && r.chain == 0 && r.chain_left == 0);
    KV_CHECK(c, chain_pay(r, 90, 100) == 100 && r.chain == 1 && r.chain_best == 10);

    // A graze holds the window 20 ticks more, never past its length; with no chain it does nothing.
    for (int t = 0; t < 50; ++t) chain_tick(r);
    chain_graze(r, 90);
    KV_CHECK(c, r.chain_left == 60);
    chain_graze(r, 90);
    chain_graze(r, 90);
    KV_CHECK(c, r.chain_left == 90);
    chain_break(r);
    chain_graze(r, 90);
    KV_CHECK(c, r.chain == 0 && r.chain_left == 0);

    // The extends: 200,000, 600,000, then every million.
    KV_CHECK(c, extend_after(kFirstExtend) == 600000 && extend_after(600000) == 1000000);
    KV_CHECK(c, extend_after(1000000) == 2000000 && extend_after(2000000) == 3000000);

    // Eggs to five; the sixth is 50,000.
    Run e{};
    e.eggs = 4;
    KV_CHECK(c, egg_gain(e) && e.eggs == 5 && e.score == 0);
    KV_CHECK(c, !egg_gain(e) && e.eggs == 5 && e.score == 50000);

    // The stage bonus: 20,000 an egg, 5,000 a bomb.
    Run b{};
    b.eggs = 3;
    b.bombs = 2;
    KV_CHECK(c, stage_bonus(b) == 70000);
}
