# kromescreens

goldberg's five screens in Krome (`docs/PRDs/Krome-Slice-4-PRD.md`, KW15), written on
sample models in the shapes goldberg has, so goldberg's own window can follow (its G16):
one screen, a page a tab, in `assets/screens.kim` out of `krome/std`, its models and
handlers in `assets/screens.lua`.

- **Inspect**: an asset's upstream and downstream as a tree, a splitter, the picked one's
  type, resolved path, converter and parameters, each parameter's rule a link that opens
  its `.berg` file.
- **Diff**: the lockfile's re-routes in a table, accept or reject each, or all of them
  behind a confirming dialog (Enter is OK, Escape cancel).
- **Status**: a cook of 10,000 steps, the game's side playing it (`src/main.cc`): a few
  running at once, their state badges and progress bars, the counts and an overall bar; the
  connected targets beside it. The table is a virtualized `each`.
- **Reports**: warnings in tabs (fallback matches, out of pack, budgets), each a table that
  sorts by a column's press and filters by what is typed; a right press opens a menu.
- **Rules**: a `.berg` file's rules; the picked one in a form (pattern, type in a dropdown,
  quality on a slider, checkboxes, free text), saved with Ctrl+S or reverted; every field
  reached by Tab.

```
build/linux-dev/bin/kromescreens                       # the screens; --theme light for the other theme
build/linux-dev/bin/kromescreens --demo                # each page by pointer and keys, checks the model: "demo passed"
build/linux-dev/bin/kromescreens --demo --shots DIR    # and a picture of each page (the cook held still), two in light
build/linux-dev/bin/kromescreens --bench --pacing continuous --prof-report
                                                       # all 10,000 steps sent every tick for ten seconds
```

The numbers (Linux, dev build, 2026-11-15), the 10,000 steps sent every tick as a KBOR
list: making the document on the game's side 6.4 ms (437 KB); Krome taking it (decoding it
into Lua, building the rows in view again, laying out) 2.2 ms a frame on average, 8.8 ms
at worst; painting too small to show; 121 frames a second throughout.
