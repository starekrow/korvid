# kvimagetool

The image tool, `bin/kvimagetool`. It compares screenshots picture by picture within a
tolerance, writes their differences as pictures, and turns screenshots into PNGs to keep
as references. It is built the way every Korvid tool is (see `modules/tool`): its verbs
are CLI commands, its work a system reading and writing through the I/O queue. It is
built for the host (`platforms = { "host" }`) and links no system library: its closure is
`core`, `platform`, `cli`, `tool`, `inflate` and `image`. It replaces
`tools/ppm_compare.py`, so comparing pictures needs nothing the engine does not build.

## Using it

```bash
kvimagetool compare A B                     # two pictures, or two directories by name
kvimagetool compare A B --diff DIR          # and each pair that differs at all, amplified x8, as DIR/<name>.diff.png
kvimagetool compare A B --threshold 8 --max-fraction 0.005 --max-channel 255
kvimagetool png A DIR                       # every picture of A as DIR/<name>.png
kvimagetool help
```

A and B are `.ppm` (binary P6: what `--screenshot` and `gallery` write) or `.png`
(any the image module reads; references are kept as PNGs, a few to a hundred kilobytes
where a PPM is 1.5 MB). Given two directories, every picture in A (its own files, not its
subdirectories') meets the one of the same name in B, `.ppm` before `.png`; given a file
for B, every picture in A meets it. A PNG's alpha is dropped: the comparison is of RGB.

For each pair it prints one line:

```
ok   gallery_filter               largest channel difference   1, 0 pixels past 8 (0.0000%)
FAIL gallery_blends               largest channel difference 190, 34100 pixels past 8 (6.5779%); first at (37, 30): (230, 200, 60) against (40, 90, 200)
```

and then `N of M pictures within tolerance`. A pair passes when the fraction of its pixels
whose largest channel difference passes `--threshold` (8) is at most `--max-fraction`
(0.005) and no channel differs by more than `--max-channel` (255: no limit). A different
size fails, as does a picture B lacks. Two GPUs round linear filtering and blending
differently and an edge may land a pixel over, so a few pixels off by a little are
expected; a wrong blend, a missing layer, a sort out of order or a picture upside down
changes many pixels by a lot. The defaults are a first guess, to be fixed by the first
comparison of two backends and not loosened after (`docs/PRDs/Engine-Slice-7-PRD.md`, R9).

Engine flags go first: `kvimagetool --verbose compare ...`. The exit status is 0 when every
pair passes and everything was written, 1 when a pair failed or a file would not read,
decode or write, 2 for a command line the CLI refused.

**Linked into a game**, the module adds the same commands to its CLI.

## How it works

A command checks its arguments, lists A (and B) with `io_list_files`, pairs the pictures
by name and starts a job; `kvimagetool.pump` (PreSim, main thread, every tick) runs it:
every file is read through the I/O queue and decoded to RGB8 as it arrives; once all
are in, each pair is compared (or each picture encoded), its line printed and any
picture to write submitted as a write; when the writes are done, the summary is printed
and `tool_finish` gives the exit status.

## Quick reference

Commands: `kvimagetool compare <a> <b> [--threshold N] [--max-fraction F] [--max-channel N]
[--diff DIR]`, `kvimagetool png <a> <dir>`.

Header `korvid/kvimagetool/module.h`: `kvimagetool_module`, the record; kormake puts it
in the list.

What the module registers with the engine: the two commands, the `.pump` system and its
state.
