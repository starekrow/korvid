# Arcfire Slice 6 — Orbit and the battle

Nov 10, 2026 · @starekrow

## Goal and exit criteria

The last slice. It adds the second and last stage, Orbit: no ground, a fleet engagement
half an orbit away as its background, a nebula drawn by a shader of the game's own, and a
capital ship coming apart over the stage. It gives the engine its first game shaders,
three of them, required: two on sprites (the nebula, the dissolve) and one over the whole
frame (heat shimmer around explosions), which needs the renderer to draw the world into a
target of its own first. They are also this slice's port check: a game shader goes through
kormake's shader rule, the renderer's pipelines and each platform's Vulkan driver, so a
stage that draws with three of them is a test a port must pass. The slice closes with the
platform checklist below run on Linux and Windows (macOS when it has a port), the
notebook's closing entry, and a README that tells someone else how to watch the game work.

It is done when:

- A material can name a fragment shader of the game's own (`shaders/*.slang` in the game's
  module, compiled by the build as the engine's are), drawn with the engine's sprite vertex
  shader, its blend, its texture, its material's parameters, the sprite's tint and a time;
  a shader that will not load falls back to the engine's, logged.
- Three game shaders draw: the **nebula** (a procedural field of layered noise on Orbit's
  deep plane, seeded by the stage, scrolling), the **dissolve** (a sprite burning away along
  a noise edge, its progress in the tint's alpha: the capital ship's breaking up, and every
  boss's death after it) and the **shimmer** (a post-process over the world, not the HUD:
  the air bending in a ring around each big explosion as it spreads and fades).
- The renderer can draw the world into a color target of its own and run a game's
  full-frame shader over it before the layers above it (the HUD, the UI) are drawn.
- Orbit is a stage: its script (formations, the station's segments drifting through the
  middle planes as its ground rows, the densest patterns), a gunship pair as mid-boss, a
  station core as boss with the game's last three phases, its music. Arcade plays Shoreline
  then Orbit before going round again; stage select offers both.
- The battle: hundreds of small ships in two colors trading tracers on the far planes,
  flashes, the capital ship breaking up; all cosmetic, a `fleet` dial, measured at stress.
- The platform checklist (below) is run on Linux and Windows and recorded in the notebook,
  each platform's hash matrix passing on its own and its recorded runs its own.
- The demo still plays Shoreline (Orbit has a demo of its own, `--demo --stage orbit`, hashing as
  the other does); the README, the credits and the notebook are current; validation is
  clean; the leak report is empty.

## Where slice 5 and the tuning left it

- **One stage.** Shoreline, a Tiled map under clouds; the stage file is read at startup and
  the run's world row and planes are built from it. Arcade goes round Shoreline again a
  difficulty up; the stage card says "STAGE 1".
- **Materials** (`render/material.h`): a texture, a blend (opaque, alpha, additive), a tint,
  four parameters, a filter; `.material.json` files or code. The renderer has one pipeline
  per blend, the engine's `sprite.slang` behind them all, and a GPU sort into buckets.
- **Shaders**: `.slang` compiled to SPIR-V by kormake's `shader` rule (the `slang` module,
  `slangc` from the system), loaded from `<exe>/shaders/` at startup, packaged by `kormake
  package`. Only the engine has any.
- **Ports**: Linux is where everything is built and measured. Windows builds the renderer
  and every example (MSVC, /W4 /WX, a native Win32 window, the sync I/O backend); arcfire's
  slices 2 to 5 have not been run there. macOS is the requirements' third platform, through
  KosmicKrisp, and has no toolchain yet.
- **The tuning pass**: a death costs a power level; the shaped Beam; heavies and the missile
  boat; bosses three times as hard. The bot clears Shoreline at every difficulty.

## Game shaders (the engine's part)

**A material names a shader**: `MaterialDesc::shader` (and `"shader"` in a material file),
the name of a SPIR-V file in `<exe>/shaders/`, a fragment shader with one entry point. The
renderer keeps a pipeline per (shader, blend), made when a material first names the pair;
the sort's bucket key gains the pipeline, so a game shader's sprites batch as the engine's
do. Nothing else of the sprite path changes: the instance data, the vertex shader and the
bindless texture table are the engine's.

**What a fragment shader gets** (`sprite_common.slangh`, made includable by game shaders):
the interpolated uv, the sprite's tint, its material's index (so its texture and its four
parameters), its world position, and a frame block with the render time in seconds and the
sim tick. A shader is presentation: nothing it computes reaches the sim or the hash, and
time-driven effects use the render time, so a paused game's nebula still moves.

**The build**: a game module with `shaders/*.slang` gets the `shader` rule as the engine's
modules do; outputs land in `<exe>/shaders/` under the module's prefix
(`example_arcfire.nebula.spv`), and `kormake package` takes them with the rest. `slangc` is
a tool dependency on each platform (a system one where there is one, a vendored binary
otherwise); the runtime compiles nothing.

**Failure**: a shader that is missing, does not load or fails pipeline creation is logged
once by name and its materials draw with the engine's shader. A port whose driver rejects
a game shader shows a plain sprite, not a crash.

## The full-frame pass (the engine's part, second)

**A frame in two parts.** Layers below a boundary the game sets (`renderer_post_layer`: the
world's, here everything under the HUD) are drawn into an offscreen color target the size
of the swapchain image; a full-frame pass then draws that target to the swapchain image
through the game's post shader; the layers at and above the boundary draw over it as now.
With no post shader set, the frame is drawn as now, straight to the swapchain image, at no
cost.

**What a post shader gets**: the world's target (sampled), the frame block (the time, the
target's size), and a parameter block the game fills each frame (`renderer_post_params`:
up to 16 vec4s). It is presentation, as the sprite shaders are.

**The ports**: an offscreen target, a sampled image, a layout transition and a second
pass are exactly what drivers and swapchain formats differ on (sRGB or not, the
swapchain's own format and usage flags), which is why the user wants it in the slice.

## The three shaders

- **Nebula** (`nebula.slang`): four octaves of value noise, domain-warped once, mapped
  through a three-color ramp (the material's parameters: two colors and a density, the
  stage's seed in the fourth), over the deep plane's full-field sprite, scrolling with the
  plane's layer offset and drifting slowly with the render time. A star field (the
  existing tiling texture) over it; the battle over that.
- **Dissolve** (`dissolve.slang`): the sprite's own texture, cut away where a noise field
  falls below the tint's alpha (0 whole, 1 gone), with a bright edge a few texels wide (the
  material's color parameter) along the cut. The capital ship's sections use it as the
  stage goes on; every boss's hull uses it in its dying, Shoreline's included.
- **Shimmer** (`shimmer.slang`, the post shader): each of up to 16 rings (a center on the
  screen, a radius growing over its life, a strength fading) bends the world's image a few
  pixels along the ring's normal, with a faint brightening at its crest. A big explosion
  (a part's, a boss's, a capital ship section's, the bomb) starts one; a cosmetic row each,
  gathered into the parameter block every frame. A `shimmer` dial turns it off (and with
  it the offscreen target).

## Orbit

- **The script**: about the length of Shoreline. Formations of the heavier kinds from the
  start; debris fields (slow, tumbling, shootable, no fire) and station segments (big
  drifting rows on the middle plane, as Shoreline's ground rows, each with turrets) in
  place of the map's enemies; the missile boat twice. No Tiled map: a stage file may name
  none, and the ground layers are empty.
- **The mid-boss: a gunship pair.** Two medium boss rows at once (the boss kind holds two),
  each a hull with two turrets and a gun, crossing each other's paths; the stage waits until
  both are down or gone.
- **The boss: the station core.** A ring of shield emitters, then a weapon ring, then the
  core: the game's last three phases, its densest patterns.
- **The patterns**: five more for the library (the master PRD's "remaining patterns"),
  Orbit's: tighter rings, crossing spirals, a curtain, aimed walls, the core's finale.
- **The art**: station segments, the gunships, the station core and the capital ship drawn
  by `make_bosses.py` in the pack's style; the fleet's ships the pack's small ships, tinted
  two ways; the star field Screaming Brain Studios' as now.
- **The music**: two more of Juhani Junkala's five chiptunes, for the stage and its boss.
- **Arcade and the card**: Shoreline, then Orbit (the card says "STAGE 2"), then round again
  a difficulty up. Each stage's file is read at startup; a stage change in a run happens
  behind a cover, the run's score, eggs and weapon carried.

## The battle

A cosmetic kind `fleet` on the far planes: small ships in two colors holding lanes across
the field, drifting with the plane's scroll, firing tracers (cosmetic rows) at the other
side, a hit a flash and sometimes a burst. A `fleet` dial sets how many (Normal a few
hundred, stress several thousand). The capital ship is a few large sections on the far
plane, breaking up across the stage with the dissolve shader, a section at a time, with
bursts. Nothing of it touches the sim; the stress mode measures what a few thousand sprites
doing nothing but being drawn cost the extract, the sort and the GPU.

## The platform checklist

Run on Linux and Windows (decided: macOS waits for its port; its rows are kept for then).
What it finds goes in the notebook, what is the engine's in the improvements note. Hashes
are compared within a platform, never across (the master PRD's non-goal: cross-platform
determinism); each platform records its own runs.

| item | what is checked | why it is a port's |
|---|---|---|
| Game shaders | all three draw as on Linux; a broken one falls back | the driver's SPIR-V compiler (and, on macOS later, KosmicKrisp's translation to Metal) |
| The full-frame pass | the offscreen target, its format against the swapchain's, the layout transitions, a resize | formats and usage flags differ by driver and platform |
| The engine's shaders | the GPU sort's compute pass, its subgroup use | compute and subgroups differ most across drivers |
| GPU timing | timestamps, calibration, pipeline statistics; the `gpu` line degrades cleanly where a query is missing | KosmicKrisp and some drivers lack calibrated timestamps or statistics queries |
| Presentation | vsync, mailbox, a 120 Hz or faster display; frame pacing | the particles-at-render-rate question, unanswerable on a 60 Hz panel |
| Window | HiDPI (a Retina backing scale, Windows per-monitor DPI), resize, minimize to zero size, focus loss | the UI's pixel scale and the camera assume the window's pixel size |
| Input | keys by scancode on AZERTY and Dvorak, a pad's buttons, Cmd-Q and Alt-F4 | layouts and platform quit keys |
| Text entry | name entry with dead keys and an input method | the platform's text input |
| Per-user files | `options.json`, `scores.json` in %APPDATA% and ~/Library/Application Support; a user name with non-ASCII letters | paths and their encoding |
| Files | asset paths' case (Linux is case-sensitive; a path that works on Windows and macOS can fail there); the kpak and zip mounts; `.kvr` files with Windows paths | case and separators |
| I/O | the sync backend on Windows and macOS, the hash matrix without `--io uring` | io_uring is Linux's |
| Audio | the device on WASAPI and CoreAudio, 44.1 and 48 kHz, a device change mid-run | the platform audio layer |
| Threads and timing | the job pool on hybrid cores (Apple's P and E, Intel's), timer resolution and the idle work's slack | scheduling and sleep granularity |
| Build | MSVC /W4 /WX, Apple clang -Werror, unity builds; the ship build packaged and run | warnings and unity collisions per compiler |
| Determinism | the hash matrix (threads, packs, I/O) on each platform; the two recorded runs re-recorded per platform; Linux's tried on Windows only to note whether they hold | float and libm differ by platform |

## Requirements

| ID | Requirement | Priority |
| --- | --- | --- |
| R1 | Materials name game fragment shaders; pipelines per (shader, blend); fallback | P0 |
| R2 | kormake builds and packages a game module's shaders | P0 |
| R3 | The nebula shader | P0 |
| R4 | The dissolve shader: the capital ship and every boss's death | P0 |
| R4a | The full-frame pass: the world's target, a post shader, the layers above it | P0 |
| R4b | The shimmer shader, its rings, its dial | P0 |
| R5 | Orbit's script, debris and station segments, no map | P0 |
| R6 | The gunship pair mid-boss | P0 |
| R7 | The station core boss, three phases | P0 |
| R8 | Five more patterns | P1 |
| R9 | The battle: the fleet, tracers, the capital ship; the `fleet` dial, measured at stress | P0 |
| R10 | Arcade through both stages; stage select; a stage change in a run | P0 |
| R11 | Orbit's music and art, credited | P0 |
| R12 | `--demo --stage orbit`, hashing | P1 |
| R13 | The platform checklist on Linux and Windows | P0 |
| R14 | The notebook's closing entry; the README for someone else | P0 |

## Milestones

Each leaves the game building, running and hashing.

1. **Game shaders** (R1, R2, R3, R4): the engine's part, the nebula on a test plane and the
   dissolve on Shoreline's bosses' deaths, the fallback tested with a broken shader.
2. **The full-frame pass** (R4a, R4b): the world's target, the post shader, the shimmer on
   Shoreline's explosions, its cost measured.
3. **Orbit** (R5, R6, R7, R8, R10, R11): the stage, its enemies, its bosses, its patterns,
   arcade through both stages.
4. **The battle** (R9, R12): the fleet, the capital ship with the dissolve, Orbit's demo,
   the stress numbers.
5. **The platforms and the close** (R13, R14): the checklist on Linux and Windows, the
   closing notebook entry, the README.

## Decided

- **Three game shaders are required**: the nebula, the dissolve and the shimmer, the last a
  full-frame post-process over the world. They double as the ports' shader check.
- **No CPU nebula**: the shader is the nebula.
- **The checklist runs on Linux and Windows**; macOS waits for its port.
- **Hashes are compared within a platform**; each platform records its own runs, and
  cross-platform determinism stays a non-goal.
