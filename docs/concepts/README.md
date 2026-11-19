# Concepts

The ideas that cut across modules: how the engine thinks about code, data and time. Each
page describes how things work now, in the present tense. The reasons and the history
are in the design records under [design/](../design/), linked from each page.

| page | read it for |
|---|---|
| [Modules](modules.md) | what a module is, the tables it declares, writing one, and what happens to each table's entries when a tick runs |
| [Scheduling](scheduling.md) | read and write sets, the resource tree, how the order within a phase is solved, levels, early reads |
| [Cycles](cycles.md) | the Sim, Frame and Work cycles: when each runs, and why only the Sim writes the sim |

Not written as concept pages yet. The design record is the best source for now:

- Handles: one 64-bit value for every handle, kinds as issuers. [design/handles.md](../design/handles.md)
- Events as streams in the schedule. [design/events.md](../design/events.md), and the summary in [Modules](modules.md#handlers)
- Memory: the general allocator, arenas, scratch, pools, heaps. [design/allocators.md](../design/allocators.md)
- The profiler: zones, waits, marks, counters. [design/profiler.md](../design/profiler.md)
- The CLI: commands, mounts, the line protocol. [design/cli.md](../design/cli.md)
- The renderer: tables, the sort, the frame ring. [design/renderer.md](../design/renderer.md)
- Determinism and replays: covered for now in the [guide](../guides/frame-loop-and-scheduling.md#same-input-same-game) and in [frameloop](../../modules/frameloop/README.md#using-it)
