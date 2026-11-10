// Korvid bulletml: the compiled form, shared by the compiler and the runner.
//
// Ops are u32 words, an action's ops back to back and ending in OP_END:
//   OP_FIRE    dir_type dir_expr speed_type speed_expr bullet nparams expr...   (nparams KV_BML_NONE: an inline bullet, the frame's params)
//   OP_WAIT    expr
//   OP_REPEAT  times_expr action nparams expr...     (nparams KV_BML_NONE: the body inherits the frame's params)
//   OP_CALL    action nparams expr...                (likewise)
//   OP_CHDIR   type expr term_expr
//   OP_CHSPEED type expr term_expr
//   OP_ACCEL   htype hexpr vtype vexpr term_expr
//   OP_VANISH
//   OP_END
// Expressions are u32 words in BmlLibrary::code, postfix, ending in E_END; a constant is
// E_CONST and its f32 bits.
#pragma once
#include "korvid/bulletml/bulletml.h"

namespace kv {

enum BmlOp : u32 { OP_FIRE = 1, OP_WAIT, OP_REPEAT, OP_CALL, OP_CHDIR, OP_CHSPEED, OP_ACCEL, OP_VANISH, OP_END };
enum BmlDirType : u32 { DIR_AIM = 1, DIR_ABSOLUTE, DIR_RELATIVE, DIR_SEQUENCE };
enum BmlSpeedType : u32 { SPEED_ABSOLUTE = 1, SPEED_RELATIVE, SPEED_SEQUENCE };
enum BmlExprOp : u32 { E_END, E_CONST, E_PARAM, E_RAND, E_RANK, E_ADD, E_SUB, E_MUL, E_DIV, E_MOD, E_NEG };
constexpr u32 kBmlExprStack = 16;

// The words an op takes, its code included.
inline u32 bml_op_len(const u32* op) {
    switch (op[0]) {
    case OP_FIRE:    return 7 + (op[6] == KV_BML_NONE ? 0 : op[6]);
    case OP_WAIT:    return 2;
    case OP_REPEAT:  return 4 + (op[3] == KV_BML_NONE ? 0 : op[3]);
    case OP_CALL:    return 3 + (op[2] == KV_BML_NONE ? 0 : op[2]);
    case OP_CHDIR:   return 4;
    case OP_CHSPEED: return 4;
    case OP_ACCEL:   return 6;
    default:         return 1;
    }
}

// Evaluates the expression at code[at]: params, rank, and the runner's stream for $rand
// (null: $rand is 0, as when folding constants).
f32 bml_eval(const u32* code, u32 at, const f32* params, u32 nparams, f32 rank, u64* rng);

} // namespace kv
