# bulletml

BulletML patterns for any shooter on the engine: documents (Kenta Cho's format, read
through the `xml` module) compiled once into a library of ops and expressions, and
plain-data runners a game steps every tick. A library, not an engine module: it registers
no ktables, systems or events. The game owns the bullets, the runners' place in its ktables,
and the systems that step them.

## How it works

**Compiling.** `bml_compile(lib, name, text, len, why, cap)` reads one document, gives its
labelled actions, bullets and fires ids, and compiles every action's body to a run of u32
ops (`FIRE`, `WAIT`, `REPEAT`, `CALL`, `CHDIR`, `CHSPEED`, `ACCEL`, `VANISH`, `END`; see
`src/bml_internal.h`). An inline action is queued and compiled after its parent, so each
action's ops are back to back. Expressions compile to postfix code; one with no `$` is
folded to a constant. Then the action graph is walked from every new action: a recursion,
or a track nested deeper than `KV_BML_DEPTH`, is an error. Errors name the element and its
line, and a document that fails leaves the library as it was. The document becomes an
entry: its actions labelled `top...`, at most `KV_BML_TRACKS` of them.

**Running.** A `BmlRunner` is plain data (no pointers; a component in the game's ktables,
hashed like any other): per track a stack of frames (action, op offset, repeats left, its
parameters' place on the track's stack), a wait, the last fire's direction and speed; the
pending changes; a PCG32 state for `$rand`; and the motion it drives (direction, speed,
and accel's part). `bml_start` sets one up for an entry, `bml_start_bullet` for a fired
bullet that has actions of its own. `bml_step` applies the pending changes to the motion,
then runs each track until it waits or ends, handing each fire to the game's callback.
A step reads only the runner, the library and the `BmlWorld` it is given (where the runner
is, where its target is, the rank), so runners step in parallel and every result is the
same on any thread. A track runs at most `KV_BML_STEP_OPS` ops and `KV_BML_STEP_FIRES`
fires a step and carries on next tick, so a runaway repeat costs a bounded tick.

## Using it

```cpp
// at load
u32 ring = bml_compile(g.lib, "ring", text, len, why, sizeof why);  // KV_BML_NONE: `why` says
// when an enemy starts firing
bml_start(g.lib, runner, ring, seed);                                 // seed: the game's, deterministic
// every tick, for every runner (in a parallel_for)
BmlWorld w{enemy_pos, ship_pos, rank};
switch (bml_step(g.lib, runner, w, on_fire, &chunk)) { ... }          // Done, Vanish, Running
// on_fire(user, const BmlFire& f): spawn a bullet at bml_velocity(f.dir, f.speed); if
// bml_bullet_actions(lib, f.bullet), give it a runner: bml_start_bullet(lib, r, f, seed)
```

A bullet that runs a program steps like an emitter, and its velocity is
`bml_velocity(r.dir, r.speed, r.mx, r.my)` after each step.

## The format, as read here

- **Elements:** `bulletml` (`type` vertical or none), `action`, `fire`, `bullet`,
  `repeat` (`times`, then `action` or `actionRef`), `wait`, `vanish`, `changeDirection`
  (`direction`, `term`), `changeSpeed` (`speed`, `term`), `accel` (`horizontal`,
  `vertical`, `term`), `actionRef`, `bulletRef` and `fireRef` with `param`s. Labels are
  the document's own. An inline bullet in a fire reads the fire's params.
- **Expressions:** numbers, `$1`..`$12`, `$rand` (uniform in [0, 1), the runner's
  stream), `$rank`, `+ - * / %`, unary minus, parentheses. Division or modulo by zero is 0.
- **Units:** world units and ticks: a speed of 2 is two units a tick, a wait of 30 is
  thirty ticks (the reference's frames). `term`s likewise.
- **Directions:** degrees, clockwise, 0 up the screen, the reference's convention for
  `type="vertical"`; in a +y-up world direction d moves along (sin d, cos d) (pinned by
  the first test). `aim` is toward the target plus the value; `absolute` is the value;
  `relative` is the runner's own direction plus the value (an enemy's is what the game
  says, 180 by default: down); `sequence` is the track's last fire plus the value, aim
  before any fire. A bullet with no direction aims.
- **Speeds:** `absolute`, `relative` (the runner's own plus the value), `sequence` (the
  track's last fire plus the value, 1 before any fire). A bullet with no speed has 1.
- **Changes** run beside the action for their `term`, one of each kind at a time (a new
  one replaces it). `sequence` changes by its value each tick; the others reach their
  value at the term's end, a direction by the shorter way round. `accel`'s horizontal is
  right and its vertical down the screen, added to the polar motion.
- **Parallel actions:** a document's `top...` actions start together, and so do a bullet's
  actions, at most `KV_BML_TRACKS` (2) of either.

## Limits

`KV_BML_DEPTH` (6) nested actions on a track, repeats and references counted (checked at
compile); `KV_BML_PARAMS` (12) params live on a track (past them, params read as 0);
65,535 repeats; a wait of ten million ticks; `KV_BML_STEP_OPS` (4,096) ops and
`KV_BML_STEP_FIRES` (1,024) fires a track a step. A `BmlRunner` is 288 bytes.

## Not in this module

`type="horizontal"` documents, the reference's `<bulletml>`-level `accel` defaults for
horizontal games, any notion of sprites, collision or lifetime (the game's), and writing
BulletML.
