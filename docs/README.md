# Korvid documentation

Korvid is a 2D game engine built out of modules, with a fixed-tick simulation that plays
the same way on every platform and every thread count. These pages take you from "what
is this" to the exact function you need. Every page links one level deeper.

## Start here

1. **[The project README](../README.md)**: what the engine is, and how to build it and
   run the examples.
2. **[Frame loops and scheduling](guides/frame-loop-and-scheduling.md)**: what happens
   between `main` and the screen, and how the engine decides when your code runs. Read
   this before writing a system.
3. **[Modules](concepts/modules.md)**: what a module declares and how to write one: a
   component, a kind, a system, a handler, timers and triggers.
4. **Read a game.** [testbed](../examples/testbed/README.md) is the smallest one that
   uses everything once. [rocks](../examples/rocks/README.md) is a whole game written the
   way the engine likes.

After that, look up what you want to do in [tasks](tasks.md). When you want to know why
the engine is built the way it is, read the [principles](principles.md).

## Find your way

| if you want | go to |
|---|---|
| to do something specific | [Tasks](tasks.md): "I want to…", with the page and the symbol |
| a walkthrough | [Guides](guides/README.md) |
| how an idea works across the engine | [Concepts](concepts/README.md) |
| one module in depth | its page in the [catalog](#modules) below; its headers have the detail |
| working code | [Examples](#examples) |
| a command-line tool | [Tools](#tools) |
| why the engine is built this way | [Principles](principles.md) |
| why one part is the way it is | [Design records](design/) (dated) and [plans](PRDs/) |
| to write or fix these docs | [DOCSTYLE.md](../DOCSTYLE.md) |

## Modules

Every module has a page (`modules/<name>/README.md`) and documented headers
(`modules/<name>/include/korvid/<name>/`). They are grouped here by the part of the
engine they belong to. Inside each group, the first ones listed are the ones a game uses
most.

**Running a program**

| module | what it is |
|---|---|
| [frameloop](../modules/frameloop/README.md) | The mechanical part of every game: options, the window and renderer from a config, the frame, and `ExitRequested`. A game's `main` is one call. |
| [tool](../modules/tool/README.md) | The engine as a tool framework: the `main` a command-line tool needs, and the rule for when it is done. |
| `cli` | The CLI: a line protocol over stdin and TCP, the commands every module declared, request ids. *No page yet.* |
| [save](../modules/save/README.md) | Game state to bytes and back, in three layers. |

**Foundation**

| module | what it is |
|---|---|
| [core](../modules/core/README.md) | The engine's floor: memory, handles, modules, the tick and its phases, the scheduler, events, kinds, jobs, timers, the profiler. |
| [math](../modules/math/README.md) | 2D math as plain structs: vectors, points, rectangles, colors, affine transforms, deterministic scalar functions. |

**The world**

| module | what it is |
|---|---|
| [sprite](../modules/sprite/README.md) | What a drawable thing is: `Transform2` and `Sprite`, the camera, layers, and the extract to the renderer. |
| [scene](../modules/scene/README.md) | The transform hierarchy: any entity can be the child of one with a `Transform2`. |
| [collision](../modules/collision/README.md) | Bodies that move and stop in the same tick, and overlaps delivered as events. |
| [anim](../modules/anim/README.md) | Animation clips and the players that run them: flipbooks, fades, motion, event tracks. |
| [tilemap](../modules/tilemap/README.md) | Tiled maps as assets, drawn as sprites and queried by cell, layer and property. |
| [level](../modules/level/README.md) | Places to be: a catalog of levels, loaded behind a fade with the sim held. |
| [bulletml](../modules/bulletml/README.md) | BulletML patterns compiled to ops, with plain-data runners a game steps every tick. |

**Words, UI and pictures**

| module | what it is |
|---|---|
| [sprite_text](../modules/sprite_text/README.md) | Words on screen: a `Text` component laid out as glyph sprites. |
| [sprite_ui](../modules/sprite_ui/README.md) | Menus, HUDs, dialogue boxes and whole screens, as trees of named nodes. |
| [krome](../modules/krome/README.md) | UI for tools and dialogs: screens in markup or Lua, laid out with rows, columns and stacks. |
| [typeset](../modules/typeset/README.md) | Paragraphs of text in a font, for everyone who draws text. |
| [truetype](../modules/truetype/README.md) | TrueType fonts through stb_truetype, compiled once for the engine. |
| [dejavu](../modules/dejavu/README.md) | DejaVu Sans, the engine's default font, and the path to load it by. |
| [patherizer](../modules/patherizer/README.md) | Paths, filled and stroked, rasterized on the CPU at the size they are drawn. |
| [svg](../modules/svg/README.md) | SVG files read into patherizer's shapes, at cook time. |
| [image](../modules/image/README.md) | PNG and PPM, both ways, as plain functions over bytes. |
| [kim](../modules/kim/README.md), [kimb](../modules/kimb/README.md) | Krome's markup, and its compiled binary form. |

**Sound**

| module | what it is |
|---|---|
| [audio](../modules/audio/README.md) | A realtime mixer fed through lock-free rings: voices, buses, streams. |

**Assets and data**

| module | what it is |
|---|---|
| [asset](../modules/asset/README.md) | Files that become engine resources: textures, materials, fonts, sounds. You get a handle back at once. |
| [asset_live](../modules/asset_live/README.md) | A dev build's link to a running `goldberg watch`: save a file and the running program reloads it. |
| [pack](../modules/pack/README.md) | The zip format as Korvid's `.kpak` packs use it. |
| [inflate](../modules/inflate/README.md) | Deflate both ways, and CRC-32. |
| [xml](../modules/xml/README.md) | XML reading, as a tree in the JSON reader's shape. |

**Scripting**

| module | what it is |
|---|---|
| [lua](../modules/lua/README.md) | Lua 5.5, built for the engine. |
| [script](../modules/script/README.md) | The `kv` libraries a tool's Lua scripts get. |

**Platform and rendering backends.** Games don't call these directly. The build picks
them for the target.

| module | what it is |
|---|---|
| [platform](../modules/platform/README.md) | The OS without a display: the file read queue and its mounts, directories, sockets. |
| [platform_hid](../modules/platform_hid/README.md) | The window, keyboard, mouse, gamepads, and the input action map. |
| [platform_audio](../modules/platform_audio/README.md) | The audio device: one playback stream on a realtime thread. |
| `touchpad` | A gamepad on a touch screen. *No page yet.* |
| [render](../modules/render/README.md) | The render interface, the shaders, and the half of the renderer the backends share. |
| [renderer-cpu](../modules/renderer-cpu/README.md) | A backend that needs no GPU. |
| `renderer-vulkan`, `renderer-metal`, `renderer-d3d12`, `renderer-cpu-raster` | The GPU backends, and the CPU renderer's pixel kernels. *No pages yet.* |
| `slang` | The shader compiler as a build module. *No page yet.* |
| `toolchain-linux`, `toolchain-windows`, `toolchain-apple`, `toolchain-android` | Each platform's compiler, flags and bindings. *No pages yet.* |

## Examples

| example | shows |
|---|---|
| [testbed](../examples/testbed/README.md) | the smallest game that uses everything once: a kind, a view system, a handler, timers, a ticketed spawn, a trigger, assets |
| [rocks](../examples/rocks/README.md) | an asteroids-like written the way the engine likes: declared sets, handlers, timers, a trigger |
| [stones](../examples/stones/README.md) | the same game written the other way round, as you might on another engine |
| [arcanum](../examples/arcanum/README.md) | a small JRPG: Tiled maps, levels, dialogue, more than one screen |
| [arcfire](../examples/arcfire/README.md) | a bullet-hell shooter, and the engine's stress test |
| [kromedemo](../examples/kromedemo/README.md) | Krome's demo screen, paced on demand; `--work` runs a Work cycle behind it |
| [kromescreens](../examples/kromescreens/README.md) | goldberg's screens in Krome |
| [gallery](../examples/gallery/README.md) | the renderer's features in fixed scenes, as screenshots to compare backends |
| [cpukiller](../examples/cpukiller/README.md) | a stress tool for the job pool, memory and tick rate |
| [sockit2m](../examples/sockit2m/README.md) | an I/O torture test, done the engine's way |
| `window` | a window and nothing else, for bringing up a platform |

## Tools

| tool | what it is |
|---|---|
| [kormake](../tools/kormake/README.md) | the build tool: modules, dependencies, cross-compiling, tests. [BUILDING.md](../tools/kormake/BUILDING.md) explains how it is built and how to read a failed build |
| [goldberg](../tools/goldberg/README.md) | the asset orchestrator: cooks a program's assets and keeps them right |
| [kpack](../tools/kpack/README.md) | builds, lists, verifies and extracts `.kpak` packs |
| [kvscript](../tools/kvscript/README.md) | Lua with the engine's modules, for asset-making scripts |
| [picture](../tools/picture/README.md) | compares screenshots within a tolerance |
| [kbor](../tools/kbor/README.md) | JSON to KBOR and back |
| [kimc](../tools/kimc/README.md) | `.kim` to `.kimb`, and checks |
