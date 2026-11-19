# Krome notes

What was measured building Krome, and how. Linux, Radeon 780M, a dev build unless said.

## Slice 1 (Nov 15, 2026)

**Layout and paint.** `kromedemo --bench --pacing continuous --prof-report`: a screen of a
thousand rows (2,000 nodes, about 13,000 glyphs) with one text reading the model, set
every tick, so the whole screen is laid out and painted again every tick. `krome.paint`
(layout, paint, the list handed over) 1.3 to 1.5 ms a tick, worst 2.5. Nothing is skipped
for being scrolled out of view yet.

**The primitive.** The same screen: the GPU draws all of it in 0.42 ms a frame (overdraw
2.1), one instanced draw for the layer. The list is rewritten every tick there; a still
list is not uploaded at all.

**A still screen.** Over 600 still frames `krome_tests` sees no layout and no paint;
`kromedemo` idle: the list is handed over once and never again.

**Pacing on demand.** `kromedemo --run-seconds N`, CPU time at 22 s less at 2 s:
on demand (`--pacing demand`, the demo's default) 0.26 s over 20 s, 1.3% of a core: four
frames a second and a poll of the platform every 4 ms. Continuous: 0.58 s, 2.9%, sixty
frames a second blocked in vsync. Here continuous is cheap already because a still Krome
screen hands the renderer nothing.

**The demo.** `kromedemo --demo` clicks through the form (buttons, the wheel, an item, the
menu and a pick, a press outside the menu, the About modal, a press under it, a disabled
button), checks the model after each, and quits: 5 s, 0 validation messages.

**On a high-DPI screen.** Layout is in logical pixels; the window's `pixel_scale()` turns
them into physical ones, and each font is rasterized at size times scale, so text at 2x is
drawn from twice the pixels, not stretched. Worth a look on a 4K panel with the desktop's
scale at 2: `kromedemo --shots DIR` and compare the glyph edges with the 1x pictures.

## Slice 6 (Nov 18, 2026)

**Save to frame, live.** `goldberg watch` on a program's roots, the program connected to it by
itself (`asset_live`), each save timed from the write to the program having it
(`tools/goldberg/tests/live_numbers.py`, ten saves each): kromescreens' screen `.kim` 9.2 ms
median (17.0 at most), the prelude `krome/std.kim` 8.9 to 12.8 (a save now and then near 200: a
watcher's or a frame's timing, not repeatable), an icon's `.svg` 7.2 to 9.0; a 512 x 512 texture
in arcfire 49.9, of which 18.6 is the script's own PNG encode. Before the engine's waker an idle
on-demand window heard a save only at its quarter-second heartbeat: the screen's median was 72 ms
with tails of 270.

**What a reload costs Krome**: one reading of the `.kimb` (KBOR to a document), `kimb_check` of it
on its own and of each screen against it, and a rebuild of each screen as a theme switch does;
the old reading is kept until no screen built before it is left. The rebuild is the same work as
opening the screen; nothing is measured apart from the numbers above.

