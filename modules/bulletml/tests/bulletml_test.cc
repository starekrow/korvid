// BulletML as this module reads it: the direction convention first (everything else is
// written against it), then a hand-computed trace of repeats, waits and sequences, then
// references and params, the changes, $rand, the budget, and what does not compile.
#include "korvid/bulletml/bulletml.h"
#include "korvid/core/test.h"
#include <math.h>
#include <string.h>

namespace {

using namespace kv;

struct Shot { u32 tick; BmlFire fire; };
struct Log { Shot shots[2048]; u32 n = 0; u32 tick = 0; };
void record(void* user, const BmlFire& f) {
    Log& l = *(Log*)user;
    if (l.n < 2048) l.shots[l.n++] = Shot{l.tick, f};
}

u32 compile(BmlLibrary& lib, const char* name, const char* text, char* why) { return bml_compile(lib, name, text, strlen(text), why, 256); }

// Steps a runner up to `ticks` ticks; returns the tick it finished on (or ticks).
u32 run(const BmlLibrary& lib, BmlRunner& r, Log& log, u32 ticks, BmlWorld w = BmlWorld{point2{0, 0}, point2{0, -100}, 0.5f}) {
    for (log.tick = 0; log.tick < ticks; ++log.tick)
        if (bml_step(lib, r, w, record, &log) != BmlStep::Running) return log.tick;
    return ticks;
}

bool near(f32 a, f32 b, f32 eps = 1e-3f) { return fabsf(a - b) <= eps; }

} // namespace

void test_bulletml(kv::TestCtx& c) {
    c.begin("bulletml");
    char why[256];

    // ---- the direction convention ----------------------------------------------------------
    // Degrees, clockwise, 0 up the screen: the reference's for type="vertical". In a +y-up
    // world, 0 moves +y, 90 moves +x, 180 moves -y, 270 moves -x. Aim is toward the target.
    KV_CHECK(c, nearly(bml_velocity(0, 2), vec2{0, 2}, 1e-5f) && nearly(bml_velocity(90, 2), vec2{2, 0}, 1e-5f));
    KV_CHECK(c, nearly(bml_velocity(180, 1), vec2{0, -1}, 1e-5f) && nearly(bml_velocity(270, 1), vec2{-1, 0}, 1e-5f));
    KV_CHECK(c, near(bml_aim(point2{0, 0}, point2{0, -5}), 180) && near(bml_aim(point2{0, 0}, point2{5, 0}), 90) &&
                near(bml_aim(point2{0, 0}, point2{-5, 5}), 315));
    // accel: horizontal right, vertical down the screen.
    KV_CHECK(c, nearly(bml_velocity(0, 0, 1, 1), vec2{1, -1}, 1e-5f));
    BmlLibrary lib;
    u32 dirs = compile(lib, "dirs", R"(<bulletml type="vertical"><action label="top">
        <fire><direction type="absolute">0</direction><bullet/></fire>
        <fire><direction type="absolute">90</direction><bullet/></fire>
        <fire><direction type="absolute">270</direction><bullet/></fire>
        <fire><bullet/></fire>
        <fire><direction type="aim">30</direction><bullet/></fire>
        <fire><direction type="relative">-10</direction><bullet/></fire>
    </action></bulletml>)", why);
    KV_CHECK(c, dirs != KV_BML_NONE);
    BmlRunner r;
    Log log;
    bml_start(lib, r, dirs, 1);
    KV_CHECK(c, run(lib, r, log, 10, BmlWorld{point2{0, 0}, point2{10, 0}, 0.0f}) == 0 && log.n == 6); // all in the first tick
    KV_CHECK(c, near(log.shots[0].fire.dir, 0) && near(log.shots[1].fire.dir, 90) && near(log.shots[2].fire.dir, 270));
    KV_CHECK(c, near(log.shots[3].fire.dir, 90) && near(log.shots[4].fire.dir, 120)); // no direction aims; aim + 30
    KV_CHECK(c, near(log.shots[5].fire.dir, 170) && near(log.shots[0].fire.speed, 1)); // relative to the runner (180)

    // ---- a trace by hand -------------------------------------------------------------------
    // Three sequence fires two ticks apart (aim 180 + 10 each time, speed 1 + 0.5 each time),
    // then the repeat ends after its last wait and one absolute fire: ticks 0, 2, 4, 6.
    u32 trace = compile(lib, "trace", R"(<bulletml type="vertical"><action label="top">
        <repeat><times>3</times><action>
          <fire><direction type="sequence">10</direction><speed type="sequence">0.5</speed><bullet/></fire>
          <wait>2</wait>
        </action></repeat>
        <fire><direction type="absolute">$rand * 0 + 45</direction><bullet/></fire>
    </action></bulletml>)", why);
    KV_CHECK(c, trace != KV_BML_NONE);
    log = Log{};
    bml_start(lib, r, trace, 7);
    u32 done = run(lib, r, log, 100);
    KV_CHECK(c, log.n == 4 && done == 6); // done on the tick of its last fire
    KV_CHECK(c, log.shots[0].tick == 0 && log.shots[1].tick == 2 && log.shots[2].tick == 4 && log.shots[3].tick == 6);
    KV_CHECK(c, near(log.shots[0].fire.dir, 190) && near(log.shots[1].fire.dir, 200) && near(log.shots[2].fire.dir, 210) && near(log.shots[3].fire.dir, 45));
    KV_CHECK(c, near(log.shots[0].fire.speed, 1.5f) && near(log.shots[1].fire.speed, 2.0f) && near(log.shots[2].fire.speed, 2.5f));

    // ---- references and params ---------------------------------------------------------------
    // An actionRef's params reach its fires; a fireRef's reach its bullet through a
    // bulletRef; a bullet's own direction reads the bulletRef's params; $rank; an inline
    // bullet in a referenced fire reads the fire's params.
    u32 refs = compile(lib, "refs", R"(<bulletml>
        <action label="top">
          <actionRef label="ring"><param>4</param><param>1 + $rank</param></actionRef>
          <fireRef label="shot"><param>33</param></fireRef>
          <fireRef label="inline"><param>7</param></fireRef>
        </action>
        <action label="ring">
          <repeat><times>$1</times><action>
            <fire><direction type="sequence">360 / $1</direction><speed>$2</speed><bulletRef label="b"><param>$2 * 2</param></bulletRef></fire>
          </action></repeat>
        </action>
        <fire label="shot"><bulletRef label="dir"><param>$1</param></bulletRef></fire>
        <fire label="inline"><bullet><direction type="absolute">$1 * 10</direction><speed>$1</speed></bullet></fire>
        <bullet label="b"><speed>9</speed></bullet>
        <bullet label="dir"><direction type="absolute">$1 + 1</direction></bullet>
    </bulletml>)", why);
    KV_CHECK(c, refs != KV_BML_NONE);
    log = Log{};
    bml_start(lib, r, refs, 3);
    run(lib, r, log, 10, BmlWorld{point2{0, 0}, point2{0, 10}, 0.5f}); // aim 0
    KV_CHECK(c, log.n == 6);
    KV_CHECK(c, near(log.shots[0].fire.dir, 90) && near(log.shots[3].fire.dir, 0) && near(log.shots[0].fire.speed, 1.5f)); // 0 + 90 each; the fire's speed wins over the bullet's
    KV_CHECK(c, log.shots[0].fire.nparams == 1 && near(log.shots[0].fire.params[0], 3.0f));
    KV_CHECK(c, near(log.shots[4].fire.dir, 34) && near(log.shots[4].fire.speed, 1)); // the bullet's own direction, in its params
    KV_CHECK(c, near(log.shots[5].fire.dir, 70) && near(log.shots[5].fire.speed, 7));
    KV_CHECK(c, !strcmp(bml_bullet_label(lib, log.shots[0].fire.bullet), "b") && !bml_bullet_actions(lib, log.shots[0].fire.bullet));

    // ---- a bullet's program: the changes, the vanish ---------------------------------------
    u32 turn = compile(lib, "turn", R"(<bulletml>
        <action label="top"><fire><direction type="absolute">0</direction><speed>1</speed><bulletRef label="t"><param>90</param></bulletRef></fire></action>
        <bullet label="t"><action>
          <changeDirection><direction type="absolute">$1</direction><term>10</term></changeDirection>
          <changeSpeed><speed>3</speed><term>4</term></changeSpeed>
          <accel><horizontal type="relative">1</horizontal><term>5</term></accel>
          <wait>20</wait>
          <changeDirection><direction type="sequence">-1</direction><term>3</term></changeDirection>
          <wait>3</wait>
          <vanish/>
        </action></bullet>
    </bulletml>)", why);
    KV_CHECK(c, turn != KV_BML_NONE);
    log = Log{};
    bml_start(lib, r, turn, 1);
    run(lib, r, log, 3);
    KV_CHECK(c, log.n == 1 && bml_bullet_actions(lib, log.shots[0].fire.bullet));
    BmlRunner b;
    bml_start_bullet(lib, b, log.shots[0].fire, 99);
    Log none;
    u32 ticks = 0;
    BmlStep st = BmlStep::Running;
    for (; ticks < 100 && st == BmlStep::Running; ++ticks) {
        st = bml_step(lib, b, BmlWorld{point2{0, 0}, point2{0, -10}, 0.0f}, record, &none);
        if (ticks == 11) KV_CHECK(c, near(b.dir, 90) && near(b.speed, 3) && near(b.mx, 1)); // all three reached their values
    }
    KV_CHECK(c, st == BmlStep::Vanish && ticks == 24 && near(b.dir, 87)); // three ticks of -1 a tick, then the vanish

    // ---- $rand: the runner's own stream ----------------------------------------------------
    u32 rnd = compile(lib, "rand", R"(<bulletml><action label="top"><repeat><times>8</times><action>
        <fire><direction type="absolute">$rand * 360</direction><bullet/></fire></action></repeat></action></bulletml>)", why);
    Log a1, a2, a3;
    bml_start(lib, r, rnd, 42); run(lib, r, a1, 2);
    bml_start(lib, r, rnd, 42); run(lib, r, a2, 2);
    bml_start(lib, r, rnd, 43); run(lib, r, a3, 2);
    bool same = a1.n == 8 && a2.n == 8, differ = false;
    for (u32 i = 0; i < 8; ++i) { same = same && a1.shots[i].fire.dir == a2.shots[i].fire.dir; differ = differ || a1.shots[i].fire.dir != a3.shots[i].fire.dir; }
    KV_CHECK(c, same && differ);

    // ---- expressions --------------------------------------------------------------------------
    u32 ex = compile(lib, "expr", R"(<bulletml><action label="top">
        <fire><direction type="absolute">2 + 3 * 4 - -1</direction><bullet/></fire>
        <fire><direction type="absolute">(2 + 3) * 4 % 7</direction><bullet/></fire>
        <fire><direction type="absolute">5 / 0 + $rank * 10</direction><bullet/></fire>
    </action></bulletml>)", why);
    Log e;
    bml_start(lib, r, ex, 1);
    run(lib, r, e, 2, BmlWorld{point2{0, 0}, point2{0, 1}, 2.0f});
    KV_CHECK(c, e.n == 3 && near(e.shots[0].fire.dir, 15) && near(e.shots[1].fire.dir, 6) && near(e.shots[2].fire.dir, 20));

    // ---- the budget ---------------------------------------------------------------------------
    u32 flood = compile(lib, "flood", R"(<bulletml><action label="top"><repeat><times>3000</times><action>
        <fire><bullet/></fire></action></repeat></action></bulletml>)", why);
    Log f;
    bml_start(lib, r, flood, 1);
    f.tick = 0;
    bml_step(lib, r, BmlWorld{}, record, &f);
    KV_CHECK(c, f.n == KV_BML_STEP_FIRES);
    f.n = 0;
    run(lib, r, f, 10);
    KV_CHECK(c, f.n == 3000 - KV_BML_STEP_FIRES);

    // ---- what does not compile, and leaves the library as it was -------------------------------
    u32 ops = lib.ops.len, entries = lib.entries.len, bullets = lib.bullets.len;
    struct Bad { const char* text; const char* says; };
    const Bad bad[] = {
        {"<bulletml><action label=\"top\"><actionRef label=\"no\"/></action></bulletml>", "no action labelled \"no\""},
        {"<bulletml><action label=\"top\"><actionRef label=\"a\"/></action><action label=\"a\"><actionRef label=\"top\"/></action></bulletml>", "calls itself"},
        {"<bulletml type=\"horizontal\"><action label=\"top\"/></bulletml>", "vertical"},
        {"<bulletml><action label=\"top\"><dance/></action></bulletml>", "not an action's element"},
        {"<bulletml><action label=\"top\"><wait>3 +</wait></action></bulletml>", "ends early"},
        {"<bulletml><action label=\"top\"><wait>$13</wait></action></bulletml>", "at most"},
        {"<bulletml><action label=\"top\"><wait>4)</wait></action></bulletml>", "')'"},
        {"<bulletml><action label=\"a\"/></bulletml>", "labelled top"},
        {"<bulletml><action label=\"top1\"/><action label=\"top2\"/><action label=\"top3\"/></bulletml>", "top actions"},
        {"<bulletml><action label=\"top\"/><action label=\"top\"/></bulletml>", "twice"},
        {"<bulletml><action label=\"top\"><fire><direction>0</direction></fire></action></bulletml>", "<bullet> or a <bulletRef>"},
        {"<bulletml><action label=\"top\"><changeSpeed><term>3</term></changeSpeed></action></bulletml>", "<speed> is missing"},
        {"<bulletml><action label=\"top\"><wait>1</wait></bulletml>", "closing tag"},
        {"<nope/>", "not <bulletml>"},
        {"<bulletml><action label=\"top\"><action><action><action><action><action><action><wait>1</wait></action></action></action></action></action></action></action></bulletml>", "nest 7 deep"},
    };
    for (const Bad& x : bad) {
        bool refused = compile(lib, "bad", x.text, why) == KV_BML_NONE && strstr(why, x.says);
        KV_CHECK(c, refused);
        if (!refused) KV_INFO("test", "expected \"%s\", got \"%s\"", x.says, why);
    }
    KV_CHECK(c, lib.ops.len == ops && lib.entries.len == entries && lib.bullets.len == bullets);
    KV_CHECK(c, compile(lib, "dirs", "<bulletml><action label=\"top\"/></bulletml>", why) == KV_BML_NONE && strstr(why, "already"));
    KV_CHECK(c, bml_find(lib, "trace") == trace && bml_find(lib, "nope") == KV_BML_NONE);
    bml_free(lib);
    KV_CHECK(c, sizeof(BmlRunner) == 288); // the README says so; the game's notebook counts it
}
