# anim

Animation clips and the players that run them. A clip is tracks over a duration with a
loop mode: a uv track is a flipbook, a tint or alpha track a fade, a position, rotation
or scale track moves a child (or a root), an event track fires named events at times.
Clips are assets, loaded from JSON files or built at runtime from a descriptor exactly
like materials, and shared by every player. An `AnimPlayer` component on any kind plays
one; the system `anim.play` advances it every tick and writes the channels onto the same
row. The fence posts are the point: the module's tests pin every one of them.

## How it works

**Clips** (`clip.h`). `anim_load(e, "mygame/flame.anim.json")` returns a handle at once
and reads the file through the I/O queue; `anim.pump` (PreSim, main thread) parses it and
`AssetReady` or `AssetFailed` follows. `anim_create(e, desc)` builds one now from an
`AnimClipDesc` (tracks of `AnimKey`s with ascending times) and is Ready at once. Either
way the record is immutable: keys in one array, tracks as runs over it, a duration (given
or the last key's time), a loop mode. `anim_sample(clip, track, time, out)` is the lookup:
step holds the last key at or before the time, linear blends with the next, both clamp
outside the keys.

```json
{ "loop": "loop",
  "tracks": [
    { "channel": "uv", "grid": [2, 1], "fps": 12, "frames": [0, 1] },
    { "channel": "scale", "interp": "linear", "keys": [[0, [1, 1]], [0.0833, [1.15, 0.9]], [0.1667, [1, 1]]] },
    { "channel": "event", "keys": [[0.1, "flicker"]] } ] }
```

A `grid` track expands to uv step keys at `i / fps` for a `cols x rows` sheet, and the
clip's duration, if not given, extends one more frame past the last key so the last
frame lasts as long as the others.

| channel | value | interp | writes |
|---|---|---|---|
| `uv` | `[x, y, w, h]` | step | `Sprite.uv` |
| `tint` | `[r, g, b, a]` | step, linear | `Sprite.tint` |
| `alpha` | `a` | step, linear | `Sprite.tint.a`, the color kept |
| `position`, `rotation`, `scale` | `[x, y]`, `r`, `[x, y]` | step, linear | `Local2` on a child kind, `Transform2` otherwise |
| `event` | `"name"` | fires | `AnimEvent{entity, clip, name}` |

**Players** (`player.h`). `AnimPlayer { clip, time, rate, clock, state, loops }`;
`anim_player(clip, rate, clock, start)` makes one for a spawn, `anim_play(p, clip, rate)`
restarts one. `anim.play` (PostSim, every tick, after `text.layout` and before
`scene.compose` when those exist) advances each playing player by `dt * rate`, segment
by segment across the clip's ends, firing event keys as it crosses them, then samples
every value track at the new time and writes the channel. The rules:

- Time stays in `[0, duration)` for `Loop` and `PingPong`. A wrap carries the remainder,
  never resets to zero, so no frame is skipped or doubled at the seam, and frame 0 is
  never shown at `t == duration`.
- A step track with keys at `t_i` shows key `i` for `[t_i, t_i+1)`; the last key holds.
- `Once` clamps at the end (or at zero in reverse), shows the last key, sets
  `AnimState::Done` and emits `AnimFinished` once. `Loop` counts `loops`. `PingPong`
  reverses `rate` at each end and counts a loop per return to the start.
- An event key fires when the interval the player moved through contains its time,
  half-open in the direction of travel (`(t0, t1]` forward, `[t1, t0)` backward) and
  closed at the start of a play and of each loop. So a key at 0 fires at the start of
  every loop and not again at the wrap, a key at the duration fires at the wrap, and
  reverse playback fires keys too.
- `AnimClock::Sim` players advance on unpaused ticks only; `Always` players on every tick
  (menus). `AnimState::Paused` holds a player on either clock.
- A player whose clip is stale or not yet Ready does nothing and keeps its time.

## Using it

```cpp
// At startup: a file clip and a built one.
g->flame_clip = anim_load(engine, "mygame/flame.anim.json");
static const AnimKey k_fade_keys[] = {{.time = 0.0f, .v = {1.0f}}, {.time = 1.0f, .v = {0.0f}}};
static const AnimTrackDesc k_fade_tracks[] = {{.channel = AnimChannel::Alpha, .interp = AnimInterp::Linear, .keys = k_fade_keys}};
AnimClipDesc fade;
fade.name = "mygame.fade"; fade.loop = AnimLoop::Once; fade.tracks = k_fade_tracks;
g->fade_clip = anim_create(engine, fade);

// A kind with an AnimPlayer; spawn with the player, at a rate that fits the moment.
ctx.spawn_later(g_debris_kind, transform2_at(at), sprite, Velocity{v}, anim_player(g.fade_clip, 1.0f / life));

// React to the end, or to a named key.
void on_anim_finished(TickContext& ctx, Span<const AnimFinished> done) {
    for (const AnimFinished& f : done) if (f.entity.kind() == g.debris_hk) ctx.destroy(f.entity);
}
void on_anim_event(TickContext& ctx, Span<const AnimEvent> evs) {
    static const u64 k_step = name_intern("step").hash;
    for (const AnimEvent& e : evs) if (e.name == k_step) ...
}
```

Add `"anim"` to the module's `deps`. A kind that plays a placement track and is a child
(`Local2`, `Parent`) animates its `Local2`; otherwise its `Transform2`.

## Quick reference

Header `korvid/anim/clip.h`:

| name | kind | what |
|---|---|---|
| `AnimChannel`, `AnimInterp`, `AnimLoop` | enums | `Uv, Tint, Alpha, Position, Rotation, Scale, Event`; `Step, Linear`; `Once, Loop, PingPong`; `*_name`, `*_parse` |
| `AnimKey` | record | `time`, `v[4]`, `event` (a `Name`) |
| `AnimTrackDesc`, `AnimClipDesc` | records | a track's channel, interp and keys; a clip's name, duration, loop and tracks |
| `AnimTrack`, `AnimClip` | records | as stored: `path`, `state`, `error`, `duration`, `loop`, `tracks`, `keys`, `players` |
| `anim_load(e, path)`, `anim_create(e, desc)` | functions | a handle at once; null (logged) for a bad descriptor |
| `anim_clip(e, h)` | function | the record, or null |
| `anim_parse(text, len, alloc, out, &err, &at)`, `anim_build(desc, alloc, out, &err)` | functions | the file format and the descriptor, as functions |
| `anim_sample(clip, track, time, out)` | function | a value track at a time; false for an event track |

Header `korvid/anim/player.h`:

| name | kind | what |
|---|---|---|
| `AnimPlayer` | component | `clip`, `time`, `rate` (1), `clock` (`Sim`), `state` (`Playing`), `loops` |
| `AnimClock`, `AnimState` | enums | `Sim, Always`; `Playing, Paused, Done` |
| `anim_player(clip, rate = 1, clock = Sim, start = 0)`, `anim_play(p, clip = null, rate = 1, start = 0)` | functions | make, restart |
| `AnimEvent{entity, clip, name}`, `AnimFinished{entity, clip}` | events | `name` is the `Name::hash`; `name_str(Name{name})` prints it |

Header `korvid/anim/module.h`: `anim_module`, `anim_stats(engine)` (`players`, `advanced`, `events`, `finished`).

What the module registers with the engine:

| name | kind | notes |
|---|---|---|
| `anim.clip` | handle kind | `kv_handle_describe` prints path, state, duration, loop, tracks, players |
| `AnimPlayer` | component | sim state |
| `AnimEvent`, `AnimFinished` | event types | with schemas: a dead entity's event is dropped |
| `anim.clips` | resource | the clip table |
| `anim.pump` | system, phase PreSim, main thread, `Always` | writes `anim.clips` |
| `anim.play` | system, phase PostSim, `Always`, after `?text.layout`, before `?scene.compose` | reads `anim.clips`; writes `*.AnimPlayer`, `*.Sprite`, `*.Local2`, `*.Transform2` |

Depends on: core, math, render, platform, asset, sprite, scene.

## Behaviour worth knowing

- **`anim.play` writes every `Sprite`, `Local2` and `Transform2` column in PostSim.** A
  game system in PostSim that writes one of those must say `after` or `before`
  `anim.play`, or the scheduler refuses the pair. Sim-phase systems are unaffected: the
  player's write lands after theirs within the tick.
- **A file clip starts when the file arrives.** A player's time is sim state; a player on
  a `Pending` clip keeps time 0 until the parse lands, on a tick that depends on I/O.
  Keep such entities in a cosmetic kind (the rocks flame) or build the clip with
  `anim_create`, which is Ready at once (the rocks debris).
- **Rate is a knob.** A one-second clip at `rate = 1 / life` lasts `life`; a negative rate
  plays backward from `start`.
- **Events arrive a boundary later**, like any event emitted from a system; a key at the
  very end of a `Once` clip and `AnimFinished` arrive together.
- **Whole-tint tracks replace the color.** Use `alpha` to fade something that keeps a tint
  of its own.

## Not in this module

Blending between clips, state machines, a `target` per track (a bone's name), skinning,
curves other than step and linear, clip events with payloads, and editors.
