# Tasks

What you might want to do, in your words, with the page that answers it and the names to
look up there. Missing something? It may not be built yet, or this list may be behind:
the [module catalog](README.md#modules) has everything.

## Getting going

- **Build the engine and run an example:** [the project README](../README.md#quick-start), then [kormake](../tools/kormake/README.md)
- **Start a new game:** [frameloop](../modules/frameloop/README.md#using-it), `FrameLoopConfig`, `frameloop_main`
- **Keep my game in its own repository:** [Modules](concepts/modules.md#the-files), `korvid.config`
- **Write a module:** [Modules](concepts/modules.md#writing-one), `ModuleDesc`, `KV_MODULE_DEFINE`
- **Write a command-line tool on the engine:** [tool](../modules/tool/README.md), `tool_main`, `tool_finish`
- **Wait on a socket or a child process without polling:** [tool](../modules/tool/README.md#a-tool-that-waits), `platform_watch_add`
- **Run a command and read its output:** [platform](../modules/platform/README.md), `Process::spawn`, `Process::shell`
- **Write a tool top to bottom, with blocking calls, and keep the engine running:** [tool](../modules/tool/README.md#straight-line-work), `tool_task`, `NetListener::accept(seconds)`, `NetSocket::read(buf, cap, seconds)`, `NetSocket::write_now`, `net_connect_local_now`, `Process::wait`
- **Know when a child process exits, without polling:** [platform](../modules/platform/README.md), `platform_watch_add`, `PlatformWatch::add(Process)`
- **Add a command-line flag to my game:** [frameloop](../modules/frameloop/README.md#using-it), `FrameLoopConfig::flags`, `frameloop_flag`, `frameloop_option`

## When code runs

- **Run code every tick:** [Frame loops and scheduling](guides/frame-loop-and-scheduling.md#placing-a-system), `SystemDesc`
- **Make one system run after another:** [Scheduling](concepts/scheduling.md#how-the-order-is-solved), `.after`, `.before`
- **Read a value before another system changes it this phase:** [Scheduling](concepts/scheduling.md#early-reads-order-or-snapshot), `^` in `reads`
- **Understand an init error about writers, cycles or torn reads:** [Frame loops and scheduling](guides/frame-loop-and-scheduling.md#when-init-refuses)
- **See the order the engine chose, and why:** [Scheduling](concepts/scheduling.md#reading-the-schedule), the `order` lines at startup
- **Run code once per frame drawn, for presentation:** [Cycles](concepts/cycles.md#the-frame-cycle-once-per-frame-drawn), `Phase::Frame`
- **Do background work in spare time:** [Cycles](concepts/cycles.md#the-work-cycle-while-there-is-work), `Phase::Work`, `ctx.more()`, `ctx.time_left()`
- **Do something N ticks from now:** [Modules](concepts/modules.md#timers-and-triggers), `ctx.schedule_in`, `ctx.cancel`
- **React when a value crosses a threshold:** [Modules](concepts/modules.md#timers-and-triggers), `TriggerDesc`, `StateEdge`
- **Pause the game, step it, or run it in slow motion:** [Cycles](concepts/cycles.md#the-sim-cycle-ticks), `engine_pause`, `engine_step`, `engine_time_scale`
- **Keep a system running while paused:** [Modules](concepts/modules.md#pause), `RunWhen::Always`
- **Hold the sim while something loads:** [Cycles](concepts/cycles.md#the-sim-cycle-ticks), `engine_hold`, `HoldLoading`
- **Spread a loop over every core:** [Modules](concepts/modules.md#systems), `parallel_for`, `parallel_reduce`
- **Quit, or react to the window closing:** [frameloop](../modules/frameloop/README.md#using-it), `ExitRequested`, `engine_request_quit`

## Entities and data

- **Define a component:** [Modules](concepts/modules.md#components), `KV_FIELDS`, `KV_COMPONENT`
- **Define a ktable:** [Modules](concepts/modules.md#ktables), `KTableDesc`
- **Spawn and destroy entities:** [Modules](concepts/modules.md#how-code-flows-through-a-tick), `ctx.spawn_later`, `ctx.destroy`, `ctx.write_ktable`
- **Give one entity data the rest of its ktable doesn't have:** [core](../modules/core/README.md#how-it-works), `ctx.attach`, `ctx.prop_set_int`
- **Name an entity and find it again:** [core](../modules/core/README.md#how-it-works), `ctx.name_later`, `engine_entity_find`
- **Send a message from one module to another:** [Modules](concepts/modules.md#events), `ctx.emit`, `HandlerDesc`
- **Share a module's data with other modules:** [Modules](concepts/modules.md#startup-shutdown-the-record), `engine_publish`, `ctx.resource<T>`
- **Allocate memory in a module:** [Modules](concepts/modules.md#memory-in-a-module), `self.arena`, `self.general`, `MemScope`
- **Start a fresh run without restarting the process:** [core](../modules/core/README.md#quick-reference), `engine_sim_reset`, `SimReset`

## Things on screen

- **Draw a sprite:** [sprite](../modules/sprite/README.md), `Transform2`, `Sprite`
- **Move, zoom or follow with the camera:** [sprite](../modules/sprite/README.md#quick-reference), `Camera2`, the resource `sprite.camera`
- **Make one thing follow another:** [scene](../modules/scene/README.md), `Parent`, `Local2`
- **Put text on screen:** [text](../modules/sprite_text/README.md), `Text`, `sptext_set`
- **Animate a sprite: flipbook, fade, motion:** [anim](../modules/anim/README.md), `anim_load`, `AnimPlayer`
- **Draw a Tiled map:** [tilemap](../modules/tilemap/README.md), `tilemap_load`, `tilemap_spawn`
- **Build a menu, a HUD or a dialogue box:** [ui](../modules/sprite_ui/README.md), `spui_load`, `spui_open`
- **Build a tool's interface:** [krome](../modules/krome/README.md)
- **Draw a skin cut from sheets, with pixel fonts and frames:** [krome](../modules/krome/README.md#how-it-works), `region`, `fit`, `frames`, `font=".kbf"`, `pixel`, `gain`
- **Write a custom shader for a material:** [render](../modules/render/README.md)
- **Take a screenshot from the command line:** [frameloop](../modules/frameloop/README.md#using-it), `--screenshot`

## Movement and collision

- **Detect when two things touch:** [collision](../modules/collision/README.md), `Collider2`, `OverlapBegin`, `OverlapEnd`
- **Move a body and have walls stop it:** [collision](../modules/collision/README.md#how-it-works), `Move2`, `MoveResult2`
- **Collide with a tile map:** [collision](../modules/collision/README.md#how-it-works), `tilemap_collide`
- **Find what is in an area, or what a shot would hit:** [collision](../modules/collision/README.md#quick-reference), `collision_query`, `collision_cast`, `collision_cast_all`

## Input and sound

- **Read controls as actions, not keys:** [platform_hid](../modules/platform_hid/README.md), `input_actions`, `action_pressed`, `action_axis`
- **Show an on-screen gamepad on a phone:** [frameloop](../modules/frameloop/README.md#quick-reference), `FrameLoopConfig::touchpad_layout`
- **Play a sound or music:** [audio](../modules/audio/README.md), `audio_play`

## Assets, levels and saves

- **Load a texture or a material:** [asset](../modules/asset/README.md), `asset_load_texture`, `asset_load_material`, `AssetReady`
- **Load and unload a group of assets together:** [asset](../modules/asset/README.md#lifetimes)
- **Ship assets in a pack:** [kpack](../tools/kpack/README.md)
- **Cook assets from sources:** [goldberg](../tools/goldberg/README.md)
- **Go from one level to another:** [level](../modules/level/README.md), `level_request`, `LevelBuild`
- **Save and load a game:** [save](../modules/save/README.md), `save_ktables`, `save_fields`

## Debugging and measuring

- **Talk to a running game from a terminal:** [frameloop](../modules/frameloop/README.md#using-it), `--cli-stdin`, `--cli-port`
- **Find where two runs stop matching:** [Frame loops and scheduling](guides/frame-loop-and-scheduling.md#same-input-same-game), `--hash`, `--hash-systems`
- **Record a run and replay it exactly:** [frameloop](../modules/frameloop/README.md#using-it), `--record`, `--replay`
- **Profile a frame:** [frameloop](../modules/frameloop/README.md#using-it), `--profile`, `--prof-report`, `KV_ZONE`
- **Catch a long frame:** [frameloop](../modules/frameloop/README.md#using-it), `--hitch-ms`
- **See where memory went, or cap a module's:** [core](../modules/core/README.md#how-it-works), `mem_report`, `HeapDesc`
- **Write tests for a module:** [core](../modules/core/README.md#quick-reference), `KV_CHECK`, `kormake test`

## Scripting

- **Script a tool in Lua:** [kvscript](../tools/kvscript/README.md), [script](../modules/script/README.md)
- **Embed Lua in a module:** [lua](../modules/lua/README.md)

## Not built yet

- **3D, physics, more animation types:** on the roadmap in [the project README](../README.md)
