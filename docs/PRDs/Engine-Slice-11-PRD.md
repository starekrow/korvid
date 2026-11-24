# Engine Slice 11 — A windowed program that is not a game

Nov 23, 2026 · @starekrow · draft

## Goal and exit criteria

Blackwing's slice 4 is its first window: a development face over the player and the
library, a session saved between runs, and the API from a script. Planning it, and
drafting its script-module spec, found nine more things it needs from the engine
(`docs/PRDs/Korvid-Changes-For-Blackwing.md` in Blackwing's checkout, KR17 to KR25).
Slice 10 took the two small ones, KR17 and KR23, with KR15 and KR16 from Blackwing's
slice 3. This slice takes the other seven. Each keeps Blackwing's ID.

Four block Blackwing's slice 4: KR20 and KR21 its first milestone, KR22 its third, KR18 its
fifth. They go first, in that order. KR24 is wanted by its sixth. KR19 and KR25 are wanted
later (script modules, album art), and are the largest; they go last.

What they have in common is a program that runs a sim that is not a game's: ticks paced by
a device's clock, a window that is often hidden, lists longer than memory wants to hold,
settings that outlive a run, and a script that drives all of it through events.

It is done when:

- A windowed program whose module asks for ticks and changes nothing on screen runs its
  Sim at the clock's rate for ten seconds and presents no frame. One that asks for
  neither wakes no more often than on-demand pacing does now.
- A module is told within a frame or two that its window was minimized, and that it was
  restored, and can read the state between; the loop draws no frame while it is hidden.
- A Lua script emits an event of a named type from a table of its fields, the rest at the
  struct's defaults, and reads a batch of a type as a list of tables, with the ktable
  binding's mapping and errors. It reads a resource by its fields.
- A list of fifty thousand rows whose model never holds more than a few hundred can be
  dragged from end to end by its scrollbar with no frame longer than a refresh, showing
  placeholders where rows have not come.
- A program can ask for its settings, data and cache directories on Linux, Windows, macOS,
  Android and iOS, and gets the place that OS's users and backup tools expect. A value set
  in the settings store, followed by a kill, is the old value or the new one at the next
  start.
- An event type and a cosmetic ktable can be registered while the engine runs, from field
  tables built at runtime, used by name, and taken away again.
- A set of real covers decodes to within rounding of a reference decoder at full size, and
  at an eighth in a small fraction of the full decode's time; a fuzzer has run over the
  decoder with the sanitizers on.
- Korvid's own tree builds as before, and the demos' hashes do not move.

## What the code has

Checked Nov 23 on `slice-10` (8d0e382).

| ID | Where | What is there now |
|---|---|---|
| KR18 | `modules/script/src/ktable.cc` | `script_push_ktable` maps a ktable's fields to Lua values and back, with the errors a script needs. Nothing for events or resources. `EventTypeDesc` has a field table and no defaults; `EventWriter::emit_bytes` and `ctx_received` take a type by id |
| KR19 | `core/src/engine.cc`, `event.h` | `engine_init` lays out every event type, ktable and resource once from the module list. `EventType<E>::id` is per C++ type. The bus's streams and each writer's buffers are arrays by type id; a writer's buffer is made on first emit |
| KR20 | `frameloop/src/frameloop.cc` | `Pacing::OnDemand`: `demand_frame` pumps and says whether to tick and draw, as one choice. With nothing to draw and Work pending it already ticks without a frame. A heartbeat draws a frame every 0.25 s |
| KR21 | `platform_hid` | `WindowResized`, `FocusChanged`, `CloseRequested`. The Win32 backend drops `SIZE_MINIMIZED`; the SDL3 backend does not read `SDL_EVENT_WINDOW_HIDDEN`, `_MINIMIZED`, `_OCCLUDED` or their opposites |
| KR22 | `krome/src/build.cc` | `each row in rows height=24` inside a scroll box builds the rows in view and a screenful each side, with spacers; the model holds the whole list |
| KR24 | `platform/src/*/paths_*.cc` | `platform_pref_path(e, org, app)`: `$XDG_DATA_HOME`, `%APPDATA%`, `~/Library/Application Support`; the phones' own storage. No settings or cache directory, no store |
| KR25 | `modules/image` | PNG and PPM only |

## A tick without a frame (KR20)

A third pacing, `Pacing::Decoupled` (`--pacing decoupled`): the loop decides ticks and
frames apart.

- **Ticks** run at the clock's rate while something wants them: `engine_want_ticks(e)`,
  asked again each tick it is still wanted (as `engine_want_frame` is), or while input,
  I/O, a load, a waker or Work is pending, or on the 0.25 s heartbeat. Between them the
  loop sleeps till the next tick is due, not a fixed 4 ms.
- **Frames** are drawn when something to draw changed: `engine_want_frame` (krome asks when
  its tree changes), input, the window's size, a load, or a tick that moved sprites. Not on
  the heartbeat, which in this pacing only ticks.
- **Hidden** (KR21): no frame at all; ticks as above.
- **A live resize or a dragged title bar**: the modal frame hook ticks on the same rule.

`OnDemand` and `Continuous` stay as they are. `frameloop_step` is unchanged.

## A hidden window (KR21)

`struct WindowVisibility { bool visible; WindowHidden why; }`, emitted when it changes,
and `HostWindow::visible()` or `platform_window_visible(e)` between. `WindowHidden`:
`None`, `Minimized`, `Occluded` (covered, another workspace, a locked screen, a suspended
surface, unmapped).

| platform | told by |
|---|---|
| SDL3 (Linux, macOS) | `SDL_EVENT_WINDOW_MINIMIZED`/`_RESTORED`, `_HIDDEN`/`_SHOWN`, `_OCCLUDED`/`_EXPOSED` (Wayland's suspended state, macOS's occlusion) |
| Win32 | `WM_SIZE` with `SIZE_MINIMIZED`, and any other size after it |
| Android | the surface's loss and return |
| iOS | the scene's background and foreground |

Where the OS cannot say, the window is shown. Every loop draws no frame while it is hidden.

## Events in Lua (KR18)

`void script_push_events(lua_State* L, Engine& e, TickContext* ctx = nullptr)` pushes an
events object; `script_events_set_context(L, index, ctx)` points it at a unit's run, and
back at null for code outside the schedule.

```lua
ev:emit("PlayerPlay", { track = 3 })    -- the rest at the struct's defaults
for _, p in ipairs(ev:received("PlayerPosition")) do ... end   -- a unit's batch
ev:fields("PlayerPlay")                  -- the field names, in order
```

- **The same mapping and errors as a row's**, in one place shared with `ktable.cc`: a
  wrong type, an integer that does not fit, a string too long, an unknown field.
- **Defaults.** `EventTypeDesc::defaults`, a pointer to a value-initialized `E` that
  `KV_EVENT` and `KV_EVENT_EX` fill in; fields the table leaves out come from it.
- **Access** is the context's: emitting a type the unit did not declare is the dev check's
  failure, reported as a Lua error.
- **Resources by their fields**: `script_push_resource(L, e, name)`, a copy of the
  resource's fields as a table, cosmetic or not (KR17's other half).

## A list over a window of rows (KR22)

`each row in rows height=24 count=@total from=@first`, inside a scroll box:

- it lays out and scrolls as if `count` rows were there;
- the model holds a run of rows in `rows`, the first of them row `from`, and may replace
  either at any time;
- when the rows it builds change (those in view and a screenful each side) it emits
  `KromeView{screen, event, first, count, total}`, and calls the screen's
  `on.<event>(first, count, total)`, so the owner can ask for those rows;
- a row in view that the model does not hold is built from the block's `placeholder`
  child, the same height;
- rows replaced under a list at rest change in place: the scroll does not move, and a row
  whose values did not change is not built again.

Fixed-height rows, as now.

## Three places, and a store (KR24)

`platform_user_dir(e, UserDir which, org, app)`, `UserDir::Settings`, `Data`, `Cache`,
made if need be, with a trailing separator. `platform_pref_path` stays as `Data`.

| | Settings | Data | Cache |
|---|---|---|---|
| Linux | `$XDG_CONFIG_HOME` | `$XDG_DATA_HOME` | `$XDG_CACHE_HOME` |
| Windows | `%APPDATA%` | `%APPDATA%` | `%LOCALAPPDATA%` |
| macOS | `~/Library/Preferences` | `~/Library/Application Support` | `~/Library/Caches` |
| iOS | `Library/Preferences` | `Library/Application Support` | `Library/Caches` |
| Android | `getFilesDir()/settings` | `getFilesDir()` | `getCacheDir()` |

Each under `<org>/<app>/`; on Windows the cache goes in `<org>/<app>/cache`.

**The store** (`settings.h`, in `platform`): named values (an integer, a number, a string,
a struct by its field table) in one file, `settings.kbor`, in the settings directory.

- **Read** at `platform` startup, before any module's startup.
- **Written whole** to a temporary name, flushed, and renamed over the old, so a crash or
  a full disk leaves the old store and never half of the new. Off the main thread, as an
  I/O task, at most one in flight; a set while one is writing is written after it.
- **Two writers** do not interleave: the write holds a lock on `settings.lock` beside it
  (`flock`, `LockFileEx`), and reads the file again under the lock and merges, last
  writer winning by key.
- **Apple's defaults system** is not used: one file everywhere is one code path, and the
  settings directory is where backups look.

## Registering after start-up (KR19)

Two calls, each at a phase boundary or between frames, from the main thread:

```cpp
u32 engine_add_event_type(Engine& e, u32 module, const EventTypeDesc& d);  // its id
u32 engine_add_ktable(Engine& e, u32 module, const RuntimeKTableDesc& d);  // a cosmetic ktable's id
void engine_remove_event_type(Engine& e, u32 id);
void engine_remove_ktable(Engine& e, u32 id);
```

- **Never sim state.** A runtime ktable is cosmetic, a runtime event type is not hashed
  and not recorded; neither changes the schedule. They are emitted and written from
  outside the schedule (`e.events`, the ktable's own calls) or by a unit that declares
  `emits = "*runtime"`, and read through `engine_events_since(e, id, cursor)`.
- **Field tables built at runtime**: the caller's memory, kept until removal.
- **Names** are the module's, and may not shadow anything registered.
- **Removal** takes the type's stream or the ktable's rows at the next boundary; the id is
  not given out again in the run. Lua's bindings to it fail with its name from then on.

## A JPEG decoder (KR25)

`image/jpeg.h`, written for the engine, as `png.h` is: a plain function over bytes,
memory from an allocator, no engine state.

```cpp
bool jpeg_info(const u8* data, usize size, JpegInfo* out);           // the header alone
bool jpeg_decode(const u8* data, usize size, const JpegOptions& opt, Allocator* a, Image* out, const char** why);
```

- **Baseline and progressive**, 8-bit, Huffman; greyscale, YCbCr at every sampling in use
  (4:4:4, 4:2:2, 4:2:0, 4:4:0, 4:1:1), restart markers. CMYK and Adobe's YCCK to RGB.
- **Scaled**: `opt.scale` 1, 2, 4 or 8, by a smaller inverse DCT (8x8 to 4x4, 2x2, 1x1),
  not by decoding whole.
- **Chroma upsampled** by interpolation (libjpeg's "fancy"), not repeated.
- **Orientation** from EXIF: applied when `opt.orient`, else reported in `out`.
- **Hostile input**: `opt.max_pixels` checked before anything is allocated; every read
  bounded; a truncated file gives the rows it had (`out->partial`) or fails with why.
- **No state between calls**: usable from any task.

Arithmetic coding, 12-bit, lossless and JPEG 2000 are out. Not vendored: libjpeg-turbo is
a large C tree with its own memory manager and SIMD build, and stb_image has no scaled
decode.

## Requirements

| ID | Requirement | Priority |
|---|---|---|
| KR20 | `Pacing::Decoupled`, `engine_want_ticks` | P0, blocks Blackwing slice 4 milestone 1 |
| KR21 | `WindowVisibility`, `platform_window_visible`, on every desktop and phone backend; no frame drawn while hidden | P0, blocks Blackwing slice 4 milestone 1 |
| KR18 | `script_push_events`, `EventTypeDesc::defaults`, `script_push_resource` | P0, blocks Blackwing slice 4 milestone 5 |
| KR22 | `each ... count= from=`, the view event, placeholders, rows replaced in place | P0, blocks Blackwing slice 4 milestone 3 |
| KR24 | `platform_user_dir`; the settings store | P1 |
| KR19 | Runtime event types and cosmetic ktables, added and removed | P1 |
| KR25 | `jpeg_info`, `jpeg_decode`: baseline, progressive, scaled, fuzzed | P1 |
| R12 | The module pages and quick references of `core`, `platform`, `platform_hid`, `frameloop`, `script`, `krome` and `image` change with their headers; `tools/doccheck.sh` reports nothing new | P0 |

## Milestones

1. **Decoupled pacing** (KR20). A test program: a module wants ticks for ten seconds and
   changes nothing; count ticks and presents. Another wants nothing; count wakes against
   on-demand.
2. **Visibility** (KR21). SDL3 built and checked by minimizing a window under Wayland and
   X11; Win32, Android and iOS written. The loop's frames counted while hidden.
3. **Events in Lua** (KR18). The shared field mapping out of `ktable.cc`; emit, received,
   fields, defaults, every error; a resource read by its fields.
4. **The sparse list, its shape** (KR22). `count` and `from`, the view event, placeholders,
   scrolling and the scrollbar over rows that are not there.
5. **The sparse list, in place** (KR22). Replacement without rebuild or scroll, and the
   fifty-thousand-row drag timed, frame by frame.
6. **Three places** (KR24, 1). Every platform's paths; Linux checked under a set and an
   unset `XDG_*`.
7. **The settings store** (KR24, 2). Values of each kind, a kill mid-write (a child process
   killed between write and rename, and during the write), two processes writing.
8. **Runtime event types** (KR19). Add, emit, read, trace, describe, remove; Lua through
   KR18's binding.
9. **Runtime ktables** (KR19). Add, spawn, write, read by name from Lua and `describe`,
   remove; never in the hash.
10. **JPEG, the decoder** (KR25). Baseline and progressive, every sampling, greyscale,
    CMYK and YCCK, restart markers, `jpeg_info`, truncated files. Compared with Pillow's
    decode of covers made by Pillow and its libjpeg-turbo.
11. **JPEG, scaled and safe** (KR25). Scaled decode, fancy upsampling, EXIF orientation,
    the speed check, and a mutation fuzzer run under `address,undefined`.

**Verification.** `kormake test` on linux-debug, linux-dev and linux-ship; the touched
modules under `--sanitize address,undefined` and `thread`; `tools/kormake/tests/run.sh`;
arcfire's and arcanum's demo hashes as recorded. Windows, macOS and the phones are written
and not verified unless a machine is to hand: the status says which.

## For review

1. **A third pacing, or OnDemand changed.** Changing OnDemand's heartbeat to tick without
   drawing would serve Blackwing as well, but goldberg's tools count on the heartbeat to
   show what the CLI patched. The draft adds `Decoupled`.
2. **One settings file, or Apple's defaults.** The draft uses a file everywhere. The
   defaults system is what macOS tools (`defaults read`) expect; it can come later behind
   the same calls.
3. **`emits = "*runtime"`.** A unit that emits runtime types declares it once, and is
   ordered against nothing for them: they are read outside the schedule only. If a runtime
   type ever needs a reader inside it, the schedule has to be built again, which this slice
   does not do.
4. **Writing the JPEG decoder.** About two thousand lines, against vendoring
   libjpeg-turbo's C (which KR14 can now build) with its memory manager bent to an
   `Allocator`. The draft writes it: one file, the engine's allocators and errors, and
   no SIMD build to carry on five toolchains.

## Status

**Milestone 1, decoupled pacing (KR20): built Nov 23, 2026 on branch `slice-11`**, which
starts from `slice-10`. As above, and:

- **The request lapses in the engine**: `engine_frame` clears `ticks_wanted` as each tick
  begins, so a pass that runs no tick (a frame drawn 4 ms after the last) keeps it.
  Cleared by the loop instead, it fizzled out under `OnDemand` within a few frames.
- **A pass that draws nothing** still checks `--run-seconds`, which sat at the bottom of
  the loop's body.
- **The last log line**, under either pacing that is not continuous: frames drawn, ticks,
  ticks without a frame, sleeps, seconds.
- **`examples/ticker`**: a module that, with `--want-ticks`, asks for every tick and
  changes nothing on screen. Ten seconds of each on this machine (Wayland, a 120 Hz
  display):

| run | frames drawn | ticks | sleeps |
|---|---|---|---|
| decoupled, ticks wanted | 1 | 600, all without a frame | 599 |
| decoupled, nothing wanted | 1 | 40 (the heartbeat) | 2,440 |
| on demand, nothing wanted | 42 | 41 | 2,495 |
| on demand, ticks wanted | 1,194 | 600 | 0 |

  The first two are KR20's test: the clock's rate with no frame after the window's first,
  and no more wakes than on demand. The last is the choice it was asked to end.
- **Not here yet:** a hidden window drawing nothing, which is milestone 2's. A live
  resize ticks and draws through the modal hook, as before, under every pacing.

**Milestone 2, visibility (KR21): built Nov 23, 2026.** As above, and:

- **One event a pump at most.** A backend sets and clears its reasons (minimized, unmapped,
  occluded) as the OS reports them, and the module sends `WindowVisibility` after the
  pump if the answer or the reason changed. Without that, an X11 restore (restored, then
  shown) was a "covered" event and a "shown" one in the same tick.
- **The frame loop draws no hidden window** under any pacing. `Continuous` ticks at the
  clock's rate and sleeps till the next is due; `OnDemand` and `Decoupled` tick as
  `Decoupled` does; the first pass after it is seen again draws.
- **`HostWindow::minimize(on)`**, for tests: SDL's minimize and restore, Win32's
  `ShowWindowAsync`; nothing on a phone. `examples/ticker` takes `--minimize-at` and
  `--restore-at`, sim ticks, and logs what the module is handed.
- **Checked on this machine** (GNOME 50, SDL 3.4.2):

| run | told | frames drawn | ticks |
|---|---|---|---|
| X11, decoupled, ticks wanted, minimized at 120, restored at 360 | hidden at 121, shown at 361 | 2 (the first, and the one after restoring) | 480 in 8 s |
| X11, on demand, minimized at 8, restored at 24 | hidden at 9, shown at 25 | 19 | 33, 16 of them while hidden |
| Wayland, decoupled, minimized at 120 | hidden at 121 | 1 | 360 in 6 s |
| X11, continuous, minimized at 120, restored at 360 | hidden at 121, shown at 361 | none while hidden | 480 in 8 s |

- **Not checked:** a window minimized by hand on Wayland, which SDL reports as occluded
  when GNOME suspends the surface, and covered windows on any desktop; Windows, macOS and
  the phones, written and not compiled. `DWMWA_CLOAKED` (another virtual desktop) is not
  asked on Windows.
- **Seen, not caused here:** continuous pacing under XWayland draws at 1 fps for about
  3 s after a restore. Presents wait about a second each, as they do for a window the X
  server has not put on screen; the cause is not confirmed.

**Milestone 3, events in Lua (KR18): built Nov 23, 2026.** As above, and:

- **One mapping.** The field code moved out of `ktable.cc` into `src/fields.cc`
  (`script_push_field`, `script_set_field`, and a table's worth of each), which the ktable,
  event and resource bindings share.
- **Defaults**: `EventDefault<E>::value`, an `E{}` with its default member initializers,
  which `KV_EVENT` and `KV_EVENT_EX` point `EventTypeDesc::defaults` at. A table without one
  (made by hand) gets zeros.
- **Checked before C++ would assert**: inside a unit, an emit of a type not in its `emits`
  and a read of one not in its `reads` are Lua errors naming the unit and the type. The
  event is built in a userdata, so an error part way emits nothing.
- **Outside the schedule** received is the running tick's window of the stream, as
  `EventBus::read` gives C++.
- **Tests** (`script events`): an event emitted in one unit from a table with two of its
  four fields, read by the next as a table and as the C++ struct; one emitted with no
  table; a wrong type, an integer that does not fit, a string too long, an unknown field,
  a table for a type without one, an undeclared emit and an undeclared read, each caught
  by `pcall` in the unit, none emitting; one emitted from outside, delivered next tick; a
  cosmetic resource read by its fields. On linux-dev, linux-debug and linux-ship, and under
  `address,undefined`.

**Milestone 4, the sparse list's shape (KR22): built Nov 23, 2026.** As above, and:

- **Markup**: `count=`, `from=` and `-> event` on an `each`, in any order after the list;
  `count=` or `from=` without `height=` is an error. `placeholder { ... }` is a child of the
  block, built for a row the model does not hold, its texts reading element 0, which no
  list has.
- **Rebuilt** when the list, the count's key or the first row's key changes, so handing
  rows over is two model writes (the list, `from`), each a region build of the rows in
  view, with the scroll left where it was.
- **Said once a change**, after the frame's layout and its virtual rounds, gathered before
  any handler runs, since a handler that sets the model rebuilds regions as it does.
- **Tests**: `krome sparse` (fifty thousand rows: the content's height; placeholders and an
  ask, once, at the start; rows handed over a frame later, in place; a jump to the middle,
  placeholders there, a new ask, rows handed over landing at the box's top with the
  scroll unmoved; half a window held, half placeholders; the count changed by the model)
  and `kim` (the markup, and the error).

**Milestone 5, the sparse list in place (KR22): built Nov 23, 2026.**

- **Once a frame.** A sparse each's keys changing marks it, and it is built again before
  the next layout (`krome_sparse_pending`). Handing rows over is two writes, the list and
  `from`, and between them the two disagree: built at each, every row was built twice and
  none could be kept.
- **Rows kept.** Each built row is recorded (`KromeSparseRow`: its row, whether the model
  held it, its element, a hash of its value, its top-level nodes). Built again, a row
  whose record matches, held or not and the same hash, has its nodes moved into place
  instead; the rest of what was built dies. The hash (`krome_model_hash`) is over the
  Lua value, a table's pairs summed so key order does not matter.
- **Renumbered.** A kept row that is now another element of the list (the window moved,
  or `from` did) has its readers' paths rewritten to it, and its texts filled from there.
- **Always built**: a row with an `each` or an `if` in its block, whose regions' frames
  name its element.
- **Tests** (`krome sparse`): the same window handed over again keeps every row node for
  node and builds only the spacers; handed over a row earlier, one row is new and the
  rest kept; one row's name changed, only that row is built again. A drag end to end over
  fifty thousand rows, 181 frames with the owner handing over each window the frame after
  it is asked for: 0.09 ms a frame on average and 0.34 ms the longest on linux-dev (the
  test holds it under 8.3 ms, a 120 Hz refresh, outside debug builds). That is Krome's
  build and layout without a renderer: the paint and the GPU are not in it. Under
  `address,undefined` too.
