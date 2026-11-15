# Idle time and I/O budgets

Nov 15, 2026: idle tasks are gone. What was one is a unit in the Work cycle
(`docs/design/cycles.md`); the I/O budget and idle priority below stand.

Nov 7, 2026. Where a frame's spare time goes, and why the I/O queue counts bytes.

## The I/O budget

Handing a read to the kernel costs time in proportion to its size: page cache allocation
on a cold read, and on a hot cache the whole copy, inside the io_uring enter. So a pump
of `IoQueue` is bounded in bytes, not requests (`IoBudget`): it admits reads in priority
order until `bytes_per_pump` would be passed (at least one always goes, so a file larger
than the budget still loads), and never more than `bytes_in_flight` at once. What is left
waits for the next pump. The synchronous backend performs reads until it has passed the
budget. Defaults: 8 MB per pump, 64 MB in flight. `pumps_capped` counts pumps that left
an admissible read waiting; `sockit2m` shows what the budget buys (`--budget`).

## Idle priority

`IoPriority::Idle` reads are never admitted by `pump()`. They wait for `pump_idle(bytes)`,
which admits anything queued, idle included, up to `bytes`. The platform module calls it
from its Work unit (`platform.io_idle`, `idle_bytes_for` the time the cycle has left) and
says `ctx.more()` while something went in. A preload for the next level, a texture
nobody is looking at yet: flag it idle and it loads when the frame has time, and only then.

## The frame loop's slack

The frame loop does not know the display's period; it knows how long the renderer blocked
last frame. `renderer_last_wait` is the seconds `renderer_render` spent in the frame fence,
the acquire and the present: with vsync, most of the period. The loop gives the Work cycle
that wait before the render (the smaller of it and its smoothed value, less
`--idle-margin-ms`, 1.5 ms), so the work eats the time the renderer would have spent
waiting for vblank rather than the time after it. With work pending it gives Work until
the frame's deadline instead, since the wait shrinks as work fills it
(`docs/design/cycles.md`, As built). Without vsync there is no wait and no slack: Work
gets one cycle a frame. `--no-idle` holds it to that; the fps line reports Work's ms per
frame.

A program that drives frames itself, as `sockit2m` does, computes its own slack from its
own deadline and hands it to `engine_work` the same way.
