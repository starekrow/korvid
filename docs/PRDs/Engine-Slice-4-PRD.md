# Engine Slice 4 — Dev PRD

Nov 7, 2026 · @starekrow

## Goal and exit criteria

Slices 1 to 3 built the engine's spine: kinds, the schedule, events, the job pool, the memory system, a GPU-driven renderer, the platform layer and a CLI. A game can be written on it, as `rocks` shows, but it cannot show a score, make a sound, pause, attach one thing to another, or play a flipbook. Slice 4 is "simple game support": everything a small 2D game needs that the engine does not yet do, each piece built in the engine's own shape (components and kinds, systems with declared sets, assets that return a handle at once, a delta stream per inner engine) so that none of it has to be rebuilt when the games stop being simple.

The spine does not change. What changes: `Sprite` learns which space it is drawn in, the delta stream carries that, the clock learns to pause, and six module-sized pieces arrive: a camera, a transform hierarchy, fonts and text, animation clips, audio, and the platform pieces a shipped game cannot do without (mouse, file writes, a per-user directory).

It is done when:

- A game pauses and resumes from a key or a pad button. While paused, the sim stands still (no timers fire, no triggers, no gameplay systems run), the menu and the HUD keep animating, the CLI keeps answering, and `pause`, `resume`, `step [N]` and `rate X` work from a terminal.
- A game sets the camera (center, zoom, rotation) and the world scrolls; sprites marked screen-space stay put in window pixels. Nothing in a game computes a view matrix.
- An entity can be the child of any other entity with a `Transform2`, in any kind. It follows its parent through position, rotation and scale, interpolates correctly, and goes with its parent when the parent dies unless it asked to be left behind.
- A `.ttf` loads as an asset at a pixel size and comes back as a glyph atlas on a renderer texture. A `Text` component on any kind with a `Transform2` renders its string as glyph sprites, left-, center- or right-aligned, in world or screen space, tinted, scaled. Changing the string is one write.
- An animation clip is an asset (JSON) or built at runtime from a descriptor, holds any number of tracks over the UV, tint, local placement and event channels, and plays on an entity through an `AnimPlayer` component with once, loop and ping-pong modes, forward or reverse. Every fencepost is tested: the last frame holds, a loop never shows frame 0 at the end, events fire exactly once per crossing in either direction, a clip that finishes says so.
- A `.wav` or `.ogg` loads as an asset. A voice plays it with a volume, a pitch and a pan, looping or not, and can be adjusted or stopped while playing. Mixing happens on the audio device's thread; the game thread never blocks on it. Voices started under the sim clock pause with it. A machine without an audio device runs the game silently.
- The mouse reaches the game as state and events, in window pixels and in screen space. A file can be written through the I/O queue. The platform names a per-user directory for saves, and `rocks` keeps its high score there.
- `rocks` shows its score and wave, pauses, has a thruster flame attached to the ship, plays shots, breaks and explosions with pitch variation, fades debris through a clip, and shows a game-over screen. `--overlay` shows the fps line and the profiler's top zones on any game.
- `rocks --demo --hash` still matches across runs and thread counts; validation stays clean; the leak report at exit is empty; every new module has its README and header docs and a test suite where the logic allows one.

## Sim control

**The clock (R1).** `TickClock` gains `paused`, `rate` and `step_ticks`. `advance` accumulates `frame_dt * rate`; while paused and with no steps pending it still produces ticks, but each one is a *paused tick*: `TickClock::tick_count` (the sim tick, what `TickContext::tick` and the timer wheel use) does not advance, `tick_total` does. `step_ticks` lets N sim ticks through a pause. `engine_pause(e, bool)`, `engine_step(e, n)` and `engine_rate(e, f)` are the API; the `cli` module exposes them as `pause`, `resume`, `step [N]` and `rate X`.

**Who runs when (R2).** `SystemDesc` gains `RunWhen runs = RunWhen::Unpaused`. On a paused tick the tick runner skips `Unpaused` systems and does not fire the timer wheel or evaluate triggers; everything else happens as on any tick: the prev copy (what the `Always` systems write still reads as motion to the extract, and the sim columns do not change, so no edge is lost), boundaries, handlers (events come only from systems that ran), the `Always` systems. The engine's own systems declare `Always` where they must: input sampling, the frame loop's exit, the asset pumps, the hierarchy, text layout, animation (which checks each player's clock), audio extract, the sprite extract, the CLI. `TickContext::paused` says which kind of tick this is. The hash is unchanged by paused ticks: `rocks --demo --hash` with a pause in the script matches a run without one.

**GPU stamps.** The sprite extract's `Tick` marker goes out on every tick, paused or not, so rows set during a pause interpolate from where they were. The renderer already counts markers itself.

## Camera and screen space

**The camera (R3).** The sprite module publishes `sprite.camera`, a `Camera2 { point2 center; f32 zoom = 1; f32 rotation = 0; }` any system may write (declared in its set), and `sprite_camera(e)` for code outside the schedule. The frame loop builds the world view from it at render rate: the window's pixel size divided by zoom, centered and rotated. Zoom 1 is one world unit per pixel, as before.

**Screen space (R4).** `Sprite` gains `SpriteSpace space = SpriteSpace::World`. `Screen` sprites are placed in window pixels, origin at the window's center, +y up, unaffected by the camera. `RenderSpriteState` carries the space; the GPU row keeps it in `flags` bit 1; the push block carries both views and both cull rects; the vertex shader and the count pass pick by the bit. Screen sprites sort with world sprites by layer and blend as before, so a HUD is simply a high layer.

## Hierarchy

**The scene module (R5).** Two components: `Parent { Handle parent; u8 orphan; }` and `Local2`, a `Placement2` relative to the parent. A kind that has `Transform2`, `Local2` and `Parent` is a child; the system `scene.compose` (PostSim, `Always`, reads `*.Transform2`, `*.Local2`, `*.Parent`, writes `*.Transform2`) writes each child's world `Transform2` as the parent's world placement composed with the child's local one. Parents may be in any kind and may themselves be children; the pass orders rows by depth (walked up the parent chain, capped at 16 with a log line and treated as a root beyond that), so a parent's world placement is final before its children read it. The engine's prev copy at PreSim makes last tick's composed transform the previous one, so interpolation needs nothing more. A child's world transform is one tick stale to Sim systems that read it (collision on a child sees last tick's placement); documented, revisited when there is a reason.

**Orphans (R6).** When the parent handle no longer resolves: `OrphanPolicy::Destroy` (the default) destroys the child through the context; `Detach` zeroes the parent handle and leaves the child where it last was, as a root. `scene_attach(table, child, parent, local)` and `scene_detach` are the helpers; the test suite covers depth order across kinds, a destroyed parent under both policies, a cycle, and interpolation of a child of a moving parent.

## Fonts and text

**Font assets (R7).** `asset_load_font(e, path, px)` returns an `asset.font` handle at once. The file (a `.ttf`, or a `.font.json` naming one with a size and a character set) reads through the I/O queue; `asset.pump_fonts` rasterizes the glyphs with stb_truetype (vendored as one translation unit in the asset module's `src/`, `STBTT_STATIC`, allocations through the module's allocator) into an 8-bit atlas packed by a shelf packer, uploads it as a renderer texture (`RGBA8_UNORM`, white with alpha, so tint is the text color), makes a material for it (alpha blend), and fills the record: glyphs by code point (uv rect, size, bearing, advance), the kern table, ascent, descent and line height. The default character set is printable ASCII plus Latin-1; a `.font.json` may list ranges. `AssetReady` arrives once the atlas is resident; before that the record has metrics of zero and text lays out as nothing, then appears. A default font ships with the text module (DejaVu Sans, under its license, in `assets/`) so the overlay and any game have one.

**The text module (R8).** `Text { Handle font; char str[KV_TEXT_MAX = 64]; f32 scale = 1; color tint; i32 layer; SpriteSpace space; TextAlign align; }` on any kind with a `Transform2` (the module declares the kind `text.label` with exactly those two). The system `text.layout` (PostSim, `Always`, reads `*.Text`, `*.Transform2`, writes `text.glyph`) keeps, per text slot, the string and look it last laid out; when they differ (or the font became ready) it destroys the slot's glyphs and spawns new ones in the kind `text.glyph` (`Transform2`, `Sprite`, `Local2`, `Parent`) as children of the text entity, one per visible character, positioned by advance and kerning from the alignment origin, each with the font's material, the glyph's uv and size times scale, the text's tint, layer and space. The hierarchy and the extract do the rest; a label that moves moves its glyphs. `text_measure(font, str, scale)` returns the width and the line metrics for a game laying out its own screens. A `Text` on a destroyed entity leaves no glyphs: the layout treats a dead slot like an emptied string.

**The overlay (R9).** `--overlay` in the frame loop: a `text.label` in screen space at the top-left, `Always`, showing the fps line; with `--prof-report`, the top zones under it. The frame loop depends on `text`.

## Animation

**Clips (R10).** An `AnimClip` is tracks over a duration with a loop mode. A track has a channel, an interpolation and keys at ascending times:

| Channel | Value | Interpolation | Writes |
| --- | --- | --- | --- |
| `uv` | rect2 | step | `Sprite.uv` |
| `tint` | color | step or linear | `Sprite.tint` |
| `position`, `rotation`, `scale` | vec2 / f32 / vec2 | step or linear | `Local2` (a child's placement; a root's `Transform2` if it has no `Local2`) |
| `event` | a name (interned) | fires on crossing | `AnimEvent { entity, clip, name }` |

A `uv` track may be written as a grid: `"grid": [cols, rows], "frames": [...], "fps": N` expands to keys at the fence posts. Clips are assets (`anim_load(e, "path.anim.json")`, read through the I/O queue and parsed in `anim.pump`, `AssetReady`/`AssetFailed` as for every asset) or built at runtime from an `AnimClipDesc` with `anim_create(e, desc)`, exactly as a material is. The record is immutable once created; a clip handle is shared by every player of it. The channel list is an enum with room; a skeleton later is more channels (a bone is a child with a `Local2`) and a `target` per track, not a new system.

**Players (R11).** `AnimPlayer { Handle clip; f32 time; f32 rate = 1; AnimClock clock; AnimState state; u32 loops; }` on any kind. The system `anim.play` (PostSim, `Always`, reads `*.AnimPlayer` and the clips, writes `*.AnimPlayer`, `*.Sprite`, `*.Local2`, `*.Transform2`) advances every playing player by `dt * rate` (players on `AnimClock::Sim` only on unpaused ticks; `Always` players on every tick), applies the loop mode, samples each track at the new time and writes the channel, and fires the event keys crossed. The rules, each one a test:

- Time lives in `[0, duration)` for `Loop` and `PingPong`; a wrap is `fmod`, never a reset to zero, so a frame is never skipped or doubled at the seam and frame 0 is never shown at `t == duration`.
- A step track with N keys over the duration shows key i for `[t_i, t_{i+1})`; the last key holds to the end.
- `Once` clamps at the duration (or at zero in reverse), shows the last key, sets `state = Done` and emits `AnimFinished { entity, clip }` once. `Loop` counts `loops`; `PingPong` reverses `rate` at each end and counts a loop per return.
- An event key fires when the interval the player moved through contains its time, half-open in the direction of travel, so reverse playback fires it too, a wrap fires the keys in both pieces of the interval exactly once, and a key at time 0 fires at the start of each loop and not again at the wrap.
- A player with a stale clip handle or a clip not yet ready does nothing and keeps its time.
- `anim_play(player, clip, rate)` restarts; `state = Paused` holds a player without the clock.

## Audio

**The device (R12).** The platform opens the audio device through SDL (`SDL_INIT_AUDIO`, a stream with a callback) at the mixer's request: `platform_audio_open(e, AudioDeviceDesc { rate 48000, channels 2, frames 512, callback, user })`, `platform_audio_close`. The callback runs on SDL's audio thread. If no device opens, the call fails, the audio module logs it once and runs without a device: commands are accepted and voices finish at once.

**Sound assets (R13).** `asset_load_sound(e, path)` returns an `asset.sound` handle; `asset.pump_sounds` decodes the file into interleaved f32 PCM at its own rate and channel count: WAV (PCM 8/16/24/32-bit and float, our own reader) and Ogg Vorbis (stb_vorbis, vendored as one translation unit in the asset module like stb_truetype). The record holds the samples, rate, channels and frame count and lives until shutdown, so the mixer thread may read it without a lock once it is `Ready`; a voice started before that waits for it.

**Voices and the mixer (R14).** The audio module publishes `audio.commands`; from a system, `audio_play(ctx, SoundPlay { sound, volume, pitch, pan, loop, bus, pause_with_sim = true })` returns a voice handle at once from a main-thread pool, `audio_set(ctx, voice, volume, pitch, pan)` and `audio_stop(ctx, voice)` follow. The system `audio.extract` (PostSim, `Always`, main thread) stamps the frame's commands with the tick and pushes them through a lock-free single-producer ring to the mixer; when the sim pauses or resumes it sends a pause or a resume for the voices that follow the sim. The mixer, in the callback: pops commands, starts voices at the beginning of the buffer (within-buffer placement by tick time is P1), reads each voice's samples at its pitch ratio with linear interpolation, applies volume, pan and the bus gain, sums into f32 stereo, and reports finished voices back through a second ring, which `audio.extract` turns into `VoiceFinished { voice }` and recycles. Four buses (`Master`, `Sfx`, `Music`, `Ui`) with a volume each. The mixer is a function over a buffer with no SDL in it, so the tests run it: a voice at pitch 2 lasts half as long, a loop joins without a click, a stop takes effect at the next buffer, pause holds a voice's position, 64 voices mix within budget.

## Platform pieces

**Mouse (R15).** `InputState` gains `MouseState { f32 x, y; u8 buttons; f32 wheel; }` in window pixels (origin top-left, as SDL reports) with `mouse_screen(window)` giving the screen-space point (center origin, +y up) that screen sprites use. Events `MouseMoved`, `MouseButtonDown`, `MouseButtonUp`, `MouseWheel`. Sampled per tick with the keys.

**Writes and the user directory (R16).** `IoQueue::write_file(IoWriteRequest { path, data, size, user })` writes (create or truncate) a whole file through the same pump, completions and backends as reads (`IORING_OP_WRITE`; `fopen` fallback), with the caller's buffer owned by the queue until the completion is reaped. `platform_pref_path(e)` is SDL's per-user directory for `FrameLoopConfig::org` and `app` (new fields, "korvid" and the title by default). `rocks` reads its high score file at startup and writes it at game over.

## The example

`rocks` gains: a `hud` label (score, wave, high score) in screen space; pause on Escape or the pad's Start, with a "PAUSED" label and a dimmed world (a screen-space full-window sprite at a high layer); `Q` or the window closing quits (`exit_on_escape = false`); a `flame` child of the ship with a two-frame uv clip at `AnimClock::Sim`, shown while thrusting; debris fading through a clip's tint track in place of the `fade` system; sounds: a shot (pitch jittered per shot), a break per rock size (three pitches of one sample), the ship's explosion, and a looping thruster voice whose volume follows the thrust; "GAME OVER" with the final score for the two seconds before the restart. The sounds are generated by a stdlib Python script next to the PNG one (a saw blip, noise bursts with decay, a looping noise bed) and one of them is also stored as `.ogg` to exercise the decoder. `--demo` scripts a pause and a resume so the hash check covers paused ticks.

## Requirements

| ID | Requirement | Priority |
| --- | --- | --- |
| R1 | Clock pause, rate and step; sim tick count holds while paused; `pause`, `resume`, `step`, `rate` in the CLI | P0 |
| R2 | `RunWhen` on systems; paused ticks skip Unpaused systems, timers, triggers and the sim prev copy; hash unaffected by pauses | P0 |
| R3 | `sprite.camera` resource; the frame loop builds the world view from it at render rate | P0 |
| R4 | `Sprite::space`; screen-space sprites through the delta stream, the GPU row, the shaders and the cull | P0 |
| R5 | `scene` module: `Parent`, `Local2`, `scene.compose` in depth order across kinds, correct interpolation | P0 |
| R6 | Orphan policies Destroy and Detach; attach and detach helpers | P0 |
| R7 | Font assets: `.ttf` at a pixel size to an atlas texture, glyph table and kerning, through the asset pipeline | P0 |
| R8 | `text` module: `Text` component laid out as glyph children; alignment; `text_measure` | P0 |
| R9 | `--overlay`: fps line and profiler zones on screen | P1 |
| R10 | Animation clips: uv, tint, position, rotation, scale and event tracks; JSON files and runtime descriptors; grid flipbooks | P0 |
| R11 | `AnimPlayer` and `anim.play`: once, loop, ping-pong, reverse, sim or always clock, `AnimEvent`, `AnimFinished`; every fencepost tested | P0 |
| R12 | Audio device through the platform with a callback; silent fallback | P0 |
| R13 | Sound assets: WAV decoded by our reader, Ogg Vorbis by stb_vorbis | P0 |
| R14 | Mixer on the device thread fed by a lock-free ring; voices with volume, pitch, pan, loop; buses; pause with the sim; `VoiceFinished` | P0 |
| R15 | Mouse state and events, window and screen coordinates | P0 |
| R16 | `write_file` on the I/O queue; `platform_pref_path`; `rocks` high score | P1 |
| R17 | `rocks` extended as described; `--demo --hash` covers a pause | P0 |
| R18 | Within-buffer placement of voice starts by tick time | P1 |
| R19 | Music streaming from disk; audio effects; spatialization | P2 |
| R20 | Text shaping, right-to-left, line wrapping, outlines; signed-distance-field glyphs | P2 |

## Out of scope for slice 4

- Hierarchy beyond placement: inherited tint, layer or visibility; a stored depth-sorted forest (the per-tick depth walk is enough until a profile says otherwise)
- Skeletal animation, blending between clips, state machines; the clip format leaves room and that is all
- Audio streaming, effects, 3D panning, more than one device
- Text beyond left-to-right Latin on one line per label; font hinting; subpixel positioning
- A UI module (buttons, focus, layout); a menu is sprites, text and the mouse
- Cooked assets, pack files, hot reload of fonts, clips or sounds
- Windows, macOS, consoles

## Open questions, risks and milestones

**Open questions**

- Whether a child's world placement should be composed at the end of Sim as well, so collision sees the current tick; the cost is a second pass.
- Whether clips should be in the asset module (one load pipeline) or in `anim` with its own small pump (whoever reads a format owns it). The PRD puts them in `anim`; a generic "load a blob for me" asset facility would settle it either way.
- Whether `Text` should keep a fixed 64 bytes or point into a string pool; 64 covers every label a simple game needs and keeps components plain data.
- Whether the audio ring should carry the engine's tick-to-time mapping so starts land sample-accurately within a buffer (R18), and how much a simple game can hear the difference at 512 frames.

**Risks**

- stb_truetype's rasterizer allocates through `STBTT_malloc`; it is pointed at the module's allocator, and the atlas build runs in the pump on the main thread, so a large character set at a large size is a visible hitch on first load. Mitigation: rasterize in idle time if it shows; the default set at 32 px is about 4 ms.
- A lock-free ring between the game thread and the audio callback is the one piece of the engine where a memory-ordering mistake is silent. It is a single producer and a single consumer with acquire/release on the indices, tested under contention in the suite, and the mixer never allocates or logs.
- Glyphs as entities put a 60-character label at 60 entities and a rewrite on every change; a score that changes every tick respawns 10 glyphs a tick. Fine for the slice; a glyph run as one GPU row is the upgrade if a game needs it.
- `RunWhen` touches the tick runner, the timer wheel and the trigger pass; a system that forgets `Always` stops during a pause, which is the right default to be wrong in.

**Milestones**

1. Sim control: the clock, `RunWhen`, the CLI commands; the mouse; the camera and screen space end to end. `rocks` pauses.
2. The `scene` module with its tests; the flame on the ship.
3. Font assets, the `text` module, the overlay; the HUD and the game-over screen.
4. Clips, players, the fencepost tests; the flame's flipbook and the debris fade.
5. The audio device, sound assets, the mixer and its tests; the sounds in `rocks`.
6. Writes and the pref path, the high score; READMEs, design notes, the final hash and validation pass.
