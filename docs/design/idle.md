# Idle time and I/O budgets

Nov 7, 2026. Where a frame's spare time goes, and why the I/O queue counts bytes.

## The I/O budget

Handing a read to the kernel costs time in proportion to its size: page cache allocation
on a cold read, and on a hot cache the whole copy, inside the io_uring enter. So a pump
of `IoQueue` is bounded in bytes, not requests (`IoBudget`): it admits reads in priority
order until `bytes_per_pump` would be passed (at least one always goes, so a file larger
than the budget still loads), and never more than `bytes_in_flight` at once. What is left
waits for the next pump. The synchronous backend performs reads until it has passed the
budget. Defaults: 8 MB per pump, 64 MB in flight. `pumps_capped` counts pumps that left
an admissible read waiting; `example_sockit2m` shows what the budget buys (`--budget`).

## Idle priority

`IoPriority::Idle` reads are never admitted by `pump()`. They wait for `pump_idle(bytes)`,
which admits anything queued, idle included, up to `bytes`. The platform module calls it
from its idle task ("platform.io", `idle_bytes_per_call` at a time) and answers More while
something went in, Nothing when nothing could. A preload for the next level, a texture
nobody is looking at yet: flag it idle and it loads when the frame has time, and only then.

## Idle tasks

`engine_idle_add(e, name, fn, user)` registers a task; `engine_idle_run(e, budget)` calls
the tasks round-robin from a cursor until the budget is used or a whole round answered
Nothing. A task slices its own work: it is called with the time left and answers More,
Nothing or Done (removed). Tasks run on the main thread under their module's memory
attribution; one that wants workers uses the pool. `Engine::idle_stats` counts runs,
calls and seconds.

## The frame loop's slack

The frame loop does not know the display's period; it knows how long the renderer blocked
last frame. `renderer_last_wait` is the seconds `renderer_render` spent in the frame fence,
the acquire and the present: with vsync, most of the period. The loop runs idle work
before the render, with a budget of the smaller of that wait and its smoothed value, less
`--idle-margin-ms` (1.5 ms), so the work eats the time the renderer would have spent
waiting for vblank rather than the time after it. Without vsync there is no wait and no
slack. `--no-idle` turns it off; the fps line reports idle ms per frame.

A game that drives frames itself, as `example_sockit2m` does, computes its own slack
from its own deadline and calls `engine_idle_run` the same way.
