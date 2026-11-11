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
- **Found, not yet fixed: the allocator strands pages under concurrent remote frees.** See
  below. Not arm64's: Linux and Windows have it.
- **Minor, left:** `kv_handle_kind_name`, `_name_hash` and `_desc_get` check only the kind
  count, which is bumped before the row is written, not the row's `live` flag. Only a
  reader racing registration with an id it got unsynchronized could see a half-written
  row; the engine registers at startup.

### The allocator's notify stacks (open)

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
same run at 36 pages. The full fix must also cover the hand-offs (pool return, orphan
adoption); `core`'s heap test frees in phases, never while the owner allocates, which is
why it has not shown.

### Still to come in later milestones

- Everything that depends on `render` waits for a backend: on macOS that is the Metal
  renderer (milestone 3). `example_cpukiller` is among them, so its tables run on macOS then.
- The window and input on Cocoa, and the shader spike (milestone 2).
