# macOS port notes

Slice 7 (docs/PRDs/Engine-Slice-7-PRD.md). Every place the engine assumed Linux or x64, what
was done, and what is still open, milestone by milestone, in the manner of
`arcanum-notes.md`. Apple silicon (arm64, 16 KiB pages), macOS 26.

## Milestone 1: ground

### The build

- **No toolchain.** kormake already bootstrapped and ran on macOS (`os_posix.c` had its
  `__APPLE__` branch); it stopped at "no toolchain module provides platform 'macos'".
  `modules/toolchain-macos` is the Linux module's rules with Apple's clang, and a new probe,
  `probe_xcode`, asks `xcrun` for the compiler, `ar` and the SDK's path, so the Ninja file
  holds them outright and no particular shell or PATH is needed. The sandbox learned the
  toolchain field `sdk`.
- **The deployment floor is macOS 26.** Homebrew's SDL3 is built for 26; linking it from a
  build that claimed 14 drew a linker warning on every executable and meant the floor was
  not real. Older releases can wait (decided Nov 10). Packaging will need an SDL3 built
  for whatever floor ships, which is the same scripted source build iOS needs.
- **`-ffp-contract=off`.** arm64 clang fuses multiply-adds by default; the x64 builds of the
  same source do not. Each platform still keeps its libm, so replays still diverge across
  platforms; the flag only keeps the compiler from adding a difference of its own.
- **A gcc-only warning.** `inflate.cc` silenced `-Wclass-memaccess`, which clang does not
  know, and clang's `-Werror` refuses an unknown warning group. The pragma is now gcc's alone.

### The posix sources

- **Sockets** (`platform/src/posix/net_posix.cc`). `SOCK_CLOEXEC`, `SOCK_NONBLOCK` and
  `accept4` do not exist on macOS. There, a socket is made plain and `fcntl` sets
  close-on-exec and non-blocking right after; an accepted socket gets the same, and
  `SO_NOSIGPIPE` once per socket replaces `MSG_NOSIGNAL` per send (the SDK declares
  `MSG_NOSIGNAL` without saying which kernels honour it, so it is not relied on). Linux is
  unchanged. `example_sockit2m` echoes 120 MB over loopback byte for byte through it.
- **Paths** (`platform/src/macos/paths_macos.cc`, new). The executable's directory from
  `_NSGetExecutablePath` and `realpath`; the preference directory
  `~/Library/Application Support/<org>/<app>/`, where SDL puts it. Inside an `.app` the
  executable's directory is `Contents/MacOS/`; finding `Resources/` from it is milestone 6's.
- **`/proc/self/exe` in an example.** `example_sockit2m` read it for its working directory
  instead of calling `platform_exe_dir`, which existed for exactly that; it calls it now,
  on every platform.
- **Page-cache eviction.** `sockit2m`'s cold pass evicts its files with `posix_fadvise`,
  which macOS lacks, and macOS has no unprivileged way to drop one file's pages. There the
  files are flushed and the eviction reports failure; the run already warns that the cold
  pass may be warm.
- **Decommit.** `MADV_DONTNEED` is advice on macOS: pages may stay resident and keep their
  bytes, where Linux's (on private anonymous memory) and Windows's `MEM_DECOMMIT` return
  zeros on the next commit. Nothing calls `os_decommit` yet, but the next caller would
  assume the zeros; macOS now maps fresh `PROT_NONE` memory over the range with
  `MAP_FIXED`, which frees the pages and keeps the reservation, and the header states the
  promise. `MAP_NORESERVE` exists on macOS (and is ignored), so reserve needed nothing.
- **16 KiB pages.** Nothing assumed 4 KiB: the small allocator carves its own 64 KiB pages
  at 64 KiB alignment, and everything else rounds with `os_page_align`.

### Weak memory

x64 orders loads with loads and stores with stores, so until now an access missing its
acquire or release could pass every test. Every lock-free structure was read against arm64:

- **Correct as written:** the job pool (its queue is under a mutex; a chunk's writes reach
  the waiter through the `ACQ_REL` increment of `done` and its `ACQUIRE` read), the handle
  registry (rows published by `live`), the mixer's command and return rings and the stream
  ring (single producer, single consumer, release on publish, acquire on read), the
  allocator's remote frees (the block's link is published by the CAS, the owner takes the
  list with an exchange), the spinlocks, and the name table (a plain `bool` behind a lock).
- **Fixed: the profiler's lap check** (`core/src/profile.cc`). The collector reads a record
  with plain loads, then reads the writer's index again to see whether the record was
  overwritten meanwhile. An acquire load keeps later loads after it, not earlier ones, so
  on arm64 the record's loads could be satisfied after the check passed. An acquire fence
  between them is the seqlock reader's rule; `atomic.h` gained `atomic_fence` for it (a
  compiler barrier on x64, `dmb` on arm64). The check was also one short on every platform:
  at `write == i + cap` the writer may be filling slot `i`, so that is lapped too, and an
  overrun ring is now read from one past its oldest slot.
- **Fixed: the allocator stranded pages under concurrent remote frees.** See below. Not
  arm64's: Linux and Windows had it too.
- **Minor, left:** `kv_handle_kind_name`, `_name_hash` and `_desc_get` check only the kind
  count, which is bumped before the row is written, not the row's `live` flag. Only a
  reader racing registration with an id it got unsynchronized could see a half-written
  row; the engine registers at startup.

### The allocator's notify stacks (fixed)

A remote free into a page whose remote list is empty pushes the page onto its owner's
notify stack, so the owner learns that a full page has blocks again. The owner also empties
remote lists in `page_take` without taking the page off the notify stack, so the next
remote free pushes it a second time while it is still there: its one `notify_next` link is
rewritten and everything below it on the stack is cut off. Those pages stay on the owner's
full list with free blocks no one reclaims. Two more windows lead to the same place: a
remote freer can still be pushing after the owner has returned the emptied page to the pool
and another thread has re-initialized it, and an adopted orphan may still be linked on its
dead owner's stack. Nothing crashes (pages are never unmapped and the walk nulls each link
as it goes); memory grows.

Measured with four threads allocating continuously and four freeing each block on another
thread, live blocks bounded at 16,384: 3,988 pages (250 MB) carved in 10 seconds and
climbing linearly. A scratch patch that lets a page be on a notify stack at most once (a
flag the remote freer sets with an exchange and the owner clears as it pops) holds the
same run at 36 pages. `core`'s heap test frees in phases, never while the owner allocates,
which is why it had not shown.

The fix (`core/src/memory.cc`) gives each page a notify state, idle, pushing, queued or
adopting, with a generation in the rest of the word:

- Only the remote free that moves a page from idle to pushing pushes it; the owner sets it
  idle as it pops it, after reading its link. A pusher's last step, pushing to queued, is a
  compare-and-swap on its own generation, so a pop that came first leaves the page idle
  and a slow pusher never mistakes a later push for its own.
- A remote freer drops the page's live count last, after any push. While the count holds
  its block the page cannot reach the pool and be re-initialized under the push, which
  closes the window where a freer wrote the link and read the class of a page another
  thread had taken.
- An emptied page goes back to the pool only when idle; a queued one waits to be collected.
- Adopting an orphan waits out a push in flight, then takes the page's notifications for
  its new owner; its link on the dead owner's stack is abandoned, and no one walks a dead
  thread's stacks (an exiting thread also drops its own, so a destructor that allocates
  later starts fresh ones).

The regression test, `core/tests/remote_free_test.cc`, needed three tries to be a test.
Its first form freed every block at once through a small ring, which put each class on one
page, and a stack with one page cannot lose another; it passed against the bug. Its second
form held a fixed delay before every free, which never freed into the page the owner was
allocating from, and the double push needs that; it passed against the bug too. The third
frees even blocks a fixed 4,096 later (many pages a class) and odd blocks at once (traffic
into the active page), with the producer held near both, so the blocks in flight are the
same on any machine in any config. Against the bug it carves 10,500 to 12,100 pages in a
round, in every config; with the fix 160 to 250 for two rounds, under a bound of 512.
(Growth after a warm-up was the wrong measure: the peak working set creeps up to its
ceiling over a million blocks or so, slower in debug.) With it, `core_tests` ran 100 times
in dev and 50 in debug and in ship with no failure.

## Milestone 2: the window and the shader spike

### The window

- **SDL3's Cocoa backend serves as is.** `src/sdl3/` and `src/sdl3_pads/` were Linux's;
  `example_window` opens through the `cocoa` video driver, logs its events and runs at the
  250 Hz it paces itself to, with gaps of about 5 ms once up (one of 27 to 104 ms while the
  window first appears). Cmd+Q already worked: SDL reports it as `SDL_EVENT_QUIT`, which the
  pump already made a `CloseRequested`.
- **Retina.** The window opened at 960x540 "px" on a 2x display: SDL3 on macOS gives a window
  its backing pixels only when asked (`SDL_WINDOW_HIGH_PIXEL_DENSITY`), and without it the
  system scales a quarter of the pixels up. Asked for on Apple; the window is now 1920x1080
  px for a 960x540 request, which is what `width()` and `height()` promise. Not on Linux,
  where at a Wayland scale it would change the framebuffer, and every measurement made there,
  by the scale squared: a decision for Linux, not a side effect of this port.
- **The Command keys.** SDL reports them (`SDL_SCANCODE_LGUI`, `RGUI`) and the engine's `Key`
  had no member for them, so they were dropped. `Key::LGui` and `RGui` (Command on a Mac,
  the Windows key on a PC; SDL's name) are appended, so every existing key keeps its number;
  Win32 maps them to E0 5B and E0 5C. Option was already `LAlt` and `RAlt`. `InputState`'s
  bit set grew from one word to two (it is sized from `Key::Count`); replays record actions
  by name, never key bits, so no recording changes.
- **`WindowSurface::Metal`** opens the window with `SDL_WINDOW_METAL`, which
  `SDL_Metal_CreateView` needs.
- **Not checked by a person yet:** dragging and resizing the window while `example_window`
  reports its gaps (a backend whose window blocks the loop during a drag shows a gap as long
  as the drag), Cmd+Q, the Command keys, Mission Control and full screen.

### The shaders

The result is `docs/design/renderer-metal.md`. In short: every shader compiles for Metal
from the same sources; the one change is the bindless texture array, which Metal cannot
express unbounded and cannot bind as a fixed array of 1024 to a stage, and which becomes an
argument buffer (a `ParameterBlock`) under `KV_TARGET_METAL`. Unset, Slang's output for every
shader is byte-identical to before. Slang compensates for Metal's `instance_id` including the
base instance. Binding indices are Slang's own, stable but implicit, so the build will carry
Slang's reflection and a dev build will check the backend's layouts against it.

Toolchain findings: Xcode 26 ships its Metal compiler as a separate download
(`xcodebuild -downloadComponent MetalToolchain`), so `xcrun metal` fails until it is
installed; the spike compiled MSL through the Metal framework at run time instead. Slang's
macOS release cannot emit SPIR-V (it has no `libslang-glslang`), which matters only for
checking Vulkan shaders on a Mac.

### Still to come in later milestones

- Everything that depends on `render` waits for a backend: on macOS that is the Metal
  renderer (milestone 3). `example_cpukiller` is among them, so its tables run on macOS then.
- The frame loop asks for a Vulkan window; it must ask for the bound renderer's surface.
