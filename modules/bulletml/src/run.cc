// Korvid bulletml: the runner. Each track of a runner is a stack of frames over the
// library's ops; a step applies the pending changes to the runner's motion, then runs each
// track until it waits, ends or uses its budget. Everything a step reads is the runner,
// the library (immutable) and the world it is given, so runners step in parallel and the
// result depends on nothing but them.
#include "bml_internal.h"
#include "korvid/core/random.h"
#include <math.h>
#include <string.h>

namespace kv {

namespace {

constexpr u64 kStream = 0xda3e39cb94b95bdbull; // the runners' PCG32 increment: one stream, the state is theirs
constexpr f32 kDeg = KV_PI / 180.0f;

f32 next_unit(u64* state) {
    Rng r;
    r.state = *state;
    r.inc = kStream;
    f32 v = r.unit();
    *state = r.state;
    return v;
}

f32 wrap360(f32 d) {
    d = fmodf(d, 360.0f);
    return d < 0.0f ? d + 360.0f : d;
}
f32 wrap180(f32 d) {
    d = wrap360(d);
    return d > 180.0f ? d - 360.0f : d;
}

struct Step {
    const BmlLibrary& lib;
    BmlRunner&        r;
    const BmlWorld&   w;
    BmlFireFn         fire_fn;
    void*             user;
    f32               aim;

    f32 eval(u32 e, const f32* params, u32 n) { return e == KV_BML_NONE ? 0.0f : bml_eval(lib.code.data, e, params, n, w.rank, &r.rng); }

    // A new frame on track t: `pn` is the op's nparams word and its expressions,
    // evaluated in the caller's params; KV_BML_NONE shares the caller's.
    void push(u32 t, u32 action, u32 left, const u32* pn, const f32* params, u32 np, u8 pbase) {
        u8& depth = r.track8[t][BML_DEPTH];
        u8& sp = r.track8[t][BML_SP];
        if (depth == KV_BML_DEPTH) return; // the compiler refuses programs that would get here
        u16* f16 = r.frame16[t][depth];
        u8* f8 = r.frame8[t][depth];
        f16[BML_ACT] = (u16)action;
        f16[BML_PC] = 0;
        f16[BML_LEFT] = (u16)(left > 0xFFFF ? 0xFFFF : left);
        f8[BML_SPB] = sp;
        if (pn[0] == KV_BML_NONE) {
            f8[BML_PBASE] = pbase;
            f8[BML_PCOUNT] = (u8)np;
        } else {
            f32 v[KV_BML_PARAMS];
            u32 n = pn[0] < KV_BML_PARAMS ? pn[0] : KV_BML_PARAMS;
            for (u32 i = 0; i < n; ++i) v[i] = eval(pn[1 + i], params, np);
            u32 room = KV_BML_PARAMS - sp;
            if (n > room) n = room; // past the stack: the rest read as 0
            memcpy(r.params[t] + sp, v, sizeof(f32) * n);
            f8[BML_PBASE] = sp;
            f8[BML_PCOUNT] = (u8)n;
            sp = (u8)(sp + n);
        }
        ++depth;
    }

    void fire(u32 t, const u32* op, const f32* params, u32 np) {
        BmlFire f;
        f.bullet = op[5];
        if (op[6] == KV_BML_NONE) { // an inline bullet: the frame's params
            f.nparams = np;
            memcpy(f.params, params, sizeof(f32) * np);
        } else {
            f.nparams = op[6] < KV_BML_PARAMS ? op[6] : KV_BML_PARAMS;
            for (u32 i = 0; i < f.nparams; ++i) f.params[i] = eval(op[7 + i], params, np);
        }
        const BmlBullet& b = lib.bullets.data[f.bullet];
        // The fire's own direction and speed, else the bullet's (in its params), else aim, 1.
        u32 dt = DIR_AIM, st = SPEED_ABSOLUTE;
        f32 dv = 0.0f, sv = 1.0f;
        if (op[1]) { dt = op[1]; dv = eval(op[2], params, np); }
        else if (b.dir_type) { dt = b.dir_type; dv = eval(b.dir_expr, f.params, f.nparams); }
        if (op[3]) { st = op[3]; sv = eval(op[4], params, np); }
        else if (b.speed_type) { st = b.speed_type; sv = eval(b.speed_expr, f.params, f.nparams); }
        bool fired = (r.flags & (BML_FIRED << t)) != 0;
        f32 dir = dt == DIR_AIM ? aim + dv : dt == DIR_ABSOLUTE ? dv : dt == DIR_RELATIVE ? r.dir + dv : (fired ? r.last[t][0] : aim) + dv;
        f32 speed = st == SPEED_ABSOLUTE ? sv : st == SPEED_RELATIVE ? r.speed + sv : (fired ? r.last[t][1] : 1.0f) + sv;
        f.dir = wrap360(dir);
        f.speed = speed;
        r.last[t][0] = f.dir;
        r.last[t][1] = speed;
        r.flags |= BML_FIRED << t;
        fire_fn(user, f);
    }

    void change(const u32* op, const f32* params, u32 np) {
        switch (op[0]) {
        case OP_CHDIR: {
            f32 v = eval(op[2], params, np);
            i32 term = (i32)eval(op[3], params, np);
            u32 T = term < 1 ? 1u : (u32)term;
            if (op[1] == DIR_SEQUENCE) r.delta[0] = v;
            else {
                f32 dest = op[1] == DIR_AIM ? aim + v : op[1] == DIR_ABSOLUTE ? v : r.dir + v;
                r.delta[0] = wrap180(dest - r.dir) / (f32)T; // the shorter way round
            }
            r.term[BML_TERM_DIR] = T;
            break;
        }
        case OP_CHSPEED: {
            f32 v = eval(op[2], params, np);
            i32 term = (i32)eval(op[3], params, np);
            u32 T = term < 1 ? 1u : (u32)term;
            r.delta[1] = op[1] == SPEED_SEQUENCE ? v : op[1] == SPEED_RELATIVE ? v / (f32)T : (v - r.speed) / (f32)T;
            r.term[BML_TERM_SPEED] = T;
            break;
        }
        case OP_ACCEL: {
            i32 term = (i32)eval(op[5], params, np);
            u32 T = term < 1 ? 1u : (u32)term;
            auto part = [&](u32 type, u32 e, f32 now) -> f32 {
                if (e == KV_BML_NONE) return 0.0f;
                f32 v = eval(e, params, np);
                return type == SPEED_SEQUENCE ? v : type == SPEED_RELATIVE ? v / (f32)T : (v - now) / (f32)T;
            };
            r.delta[2] = part(op[1], op[2], r.mx);
            r.delta[3] = part(op[3], op[4], r.my);
            r.term[BML_TERM_ACCEL] = T;
            break;
        }
        default: break;
        }
    }

    void track(u32 t) {
        if (r.wait[t] && --r.wait[t]) return;
        u32 ops_left = KV_BML_STEP_OPS, fires_left = KV_BML_STEP_FIRES;
        u8& depth = r.track8[t][BML_DEPTH];
        while (depth > 0) {
            if (ops_left-- == 0) return; // the budget: on next tick
            u16* f16 = r.frame16[t][depth - 1];
            u8* f8 = r.frame8[t][depth - 1];
            const u32* op = lib.ops.data + lib.actions.data[f16[BML_ACT]] + f16[BML_PC];
            u8 pbase = f8[BML_PBASE];
            const f32* params = r.params[t] + pbase;
            u32 np = f8[BML_PCOUNT];
            u16 len = (u16)bml_op_len(op);
            switch (op[0]) {
            case OP_FIRE:
                if (fires_left-- == 0) return;
                f16[BML_PC] = (u16)(f16[BML_PC] + len);
                fire(t, op, params, np);
                break;
            case OP_WAIT: {
                f32 v = eval(op[1], params, np);
                f16[BML_PC] = (u16)(f16[BML_PC] + len);
                if (v >= 1.0f) { r.wait[t] = v > 1e7f ? 10000000u : (u32)v; return; }
                break;
            }
            case OP_REPEAT: {
                f32 times = eval(op[1], params, np);
                f16[BML_PC] = (u16)(f16[BML_PC] + len);
                if (times >= 1.0f) push(t, op[2], times > 65535.0f ? 65535u : (u32)times, op + 3, params, np, pbase);
                break;
            }
            case OP_CALL:
                f16[BML_PC] = (u16)(f16[BML_PC] + len);
                push(t, op[1], 1, op + 2, params, np, pbase);
                break;
            case OP_CHDIR: case OP_CHSPEED: case OP_ACCEL:
                f16[BML_PC] = (u16)(f16[BML_PC] + len);
                change(op, params, np);
                break;
            case OP_VANISH:
                r.flags |= BML_VANISH;
                return;
            default: // OP_END: again, or back to the caller
                if (f16[BML_LEFT] > 1) { --f16[BML_LEFT]; f16[BML_PC] = 0; }
                else { r.track8[t][BML_SP] = f8[BML_SPB]; --depth; }
                break;
            }
        }
    }
};

} // namespace

f32 bml_eval(const u32* code, u32 at, const f32* params, u32 nparams, f32 rank, u64* rng) {
    f32 st[kBmlExprStack];
    u32 sp = 0;
    for (const u32* c = code + at;;) {
        switch (*c++) {
        case E_END: return sp ? st[sp - 1] : 0.0f;
        case E_CONST: memcpy(&st[sp++], c++, sizeof(f32)); break;
        case E_PARAM: { u32 i = *c++; st[sp++] = i < nparams ? params[i] : 0.0f; break; }
        case E_RAND: st[sp++] = rng ? next_unit(rng) : 0.0f; break;
        case E_RANK: st[sp++] = rank; break;
        case E_NEG: st[sp - 1] = -st[sp - 1]; break;
        default: {
            f32 b = st[--sp], a = st[sp - 1];
            switch (c[-1]) {
            case E_ADD: a += b; break;
            case E_SUB: a -= b; break;
            case E_MUL: a *= b; break;
            case E_DIV: a = b != 0.0f ? a / b : 0.0f; break; // never a NaN in sim state
            default:    a = b != 0.0f ? fmodf(a, b) : 0.0f; break;
            }
            st[sp - 1] = a;
            break;
        }
        }
    }
}

void bml_start(const BmlLibrary& lib, BmlRunner& r, u32 entry, u64 seed, f32 dir, f32 speed) {
    memset(&r, 0, sizeof r);
    r.rng = seed;
    next_unit(&r.rng);
    r.dir = dir;
    r.speed = speed;
    if (entry >= lib.entries.len) return;
    const BmlEntry& e = lib.entries.data[entry];
    for (u32 t = 0; t < e.count; ++t) {
        r.frame16[t][0][BML_ACT] = (u16)e.actions[t];
        r.frame16[t][0][BML_LEFT] = 1;
        r.track8[t][BML_DEPTH] = 1;
    }
}

void bml_start_bullet(const BmlLibrary& lib, BmlRunner& r, const BmlFire& fire, u64 seed) {
    memset(&r, 0, sizeof r);
    r.rng = seed;
    next_unit(&r.rng);
    r.dir = fire.dir;
    r.speed = fire.speed;
    if (fire.bullet >= lib.bullets.len) return;
    const BmlBullet& b = lib.bullets.data[fire.bullet];
    u32 n = fire.nparams < KV_BML_PARAMS ? fire.nparams : KV_BML_PARAMS;
    for (u32 t = 0; t < b.action_count; ++t) {
        r.frame16[t][0][BML_ACT] = (u16)b.actions[t];
        r.frame16[t][0][BML_LEFT] = 1;
        r.frame8[t][0][BML_PCOUNT] = (u8)n;
        memcpy(r.params[t], fire.params, sizeof(f32) * n);
        r.track8[t][BML_DEPTH] = 1;
        r.track8[t][BML_SP] = (u8)n;
    }
}

BmlStep bml_step(const BmlLibrary& lib, BmlRunner& r, const BmlWorld& w, BmlFireFn fire, void* user) {
    if (r.flags & BML_VANISH) return BmlStep::Vanish;
    if (r.term[BML_TERM_DIR]) { r.dir = wrap360(r.dir + r.delta[0]); --r.term[BML_TERM_DIR]; }
    if (r.term[BML_TERM_SPEED]) { r.speed += r.delta[1]; --r.term[BML_TERM_SPEED]; }
    if (r.term[BML_TERM_ACCEL]) { r.mx += r.delta[2]; r.my += r.delta[3]; --r.term[BML_TERM_ACCEL]; }
    Step s{lib, r, w, fire, user, bml_aim(w.pos, w.target)};
    bool running = false;
    for (u32 t = 0; t < KV_BML_TRACKS; ++t) {
        if (!r.track8[t][BML_DEPTH]) continue;
        s.track(t);
        if (r.flags & BML_VANISH) return BmlStep::Vanish;
        running |= r.track8[t][BML_DEPTH] > 0;
    }
    if (running || r.term[BML_TERM_DIR] || r.term[BML_TERM_SPEED] || r.term[BML_TERM_ACCEL]) return BmlStep::Running;
    return BmlStep::Done;
}

vec2 bml_velocity(f32 dir, f32 speed, f32 mx, f32 my) {
    f32 a = dir * kDeg;
    return vec2{sinf(a) * speed + mx, cosf(a) * speed - my};
}

f32 bml_aim(point2 from, point2 to) {
    f32 dx = to.x - from.x, dy = to.y - from.y;
    if (dx == 0.0f && dy == 0.0f) return 180.0f; // on the target: straight down the screen
    return wrap360(atan2f(dx, dy) / kDeg);
}

} // namespace kv
