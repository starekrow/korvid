# tool_kpack

The packer, `bin/kpack`. It builds a `.kpak` from a directory or a list of files, and lists, verifies,
extracts and repacks any zip. It is an engine like a game, built the way every Korvid
tool is (see `modules/tool`): its verbs are CLI commands, its work is a system reading
through the I/O queue, its memory is the engine's, and it runs on a machine with no
display. It is built for the host (`platforms = { "host" }`) and links no system library:
its closure is `core`, `platform`, `cli`, `tool`, `pack` and `inflate`.

## Using it

```bash
kpack pack out.kpak build/linux-dev/bin/assets          # everything under the directory
kpack pack out.kpak bin/assets @files.rsp               # the files a list names (paths under the root)
kpack list game.kpak                                    # every entry: size, stored size, method, CRC, offset
kpack verify game.kpak                                  # read every entry back and check its CRC-32
kpack extract game.kpak out/                            # every entry as a file
kpack repack something.zip game.kpak                    # any zip as an aligned, indexed pack
kpack help
```

Engine flags go first: `kpack --verbose --threads 4 pack ...`. The exit status is 0 on
success, 1 when the job failed (a file that would not read, a bad CRC, a write that
failed), 2 for a command line the CLI refused.

**In a build.** A game opts in with `tools = { "tool_kpack" }` in its `module.lua`. kpack's
`pack` rule runs once per such executable over exactly the assets its module closure
stages under `bin/assets/` (kormake knows the list; Ninja passes it in a response file),
writing `bin/<exe>.kpak`. The ship config builds it by default; the other configs build
it by name (`ninja -C build/linux-dev bin/example_rocks.kpak`). `kormake package
linux-ship <exe>` puts the executable, the pack, the shaders and the licenses in
`dist/<exe>/`.

**In a game.** Linked into a game, the module adds the same commands to its CLI.

## How it works

A command checks its arguments and starts a job; `tool_kpack.pump` (PreSim, main thread,
every tick) runs it in stages:

1. For `pack`: list the directory (`io_list_files`) or read the response file through
   the queue. For the read-back commands: mount the zip in the platform's queue and make
   an item per entry (the `.kpak/` names left out).
2. Submit every read at once; the queue's byte budgets spread them over pumps. Each
   completion is kept (`pack`, `repack`), checked against the table's CRC (`verify`) or
   written out at once (`extract`).
3. `pack` and `repack` build the archive with `kpak_write` and submit one write.
4. When the writes have completed, a summary line goes to stdout and `tool_finish` ends
   the run.

## Quick reference

| name | kind | what |
|---|---|---|
| `kpack pack`, `kpack list`, `kpack verify`, `kpack extract`, `kpack repack` | CLI commands | above |
| `tool_kpack.pump` | system, phase PreSim, main thread, `Always` | the job |
| `pack` | exported rule | `per = "exe"`, `inputs = "assets"`, `outputs = "%{exe}.kpak"` |

## Behaviour worth knowing

- **One job at a time.** A second command while one runs is refused.
- **The archive is built in memory** (the files and the pack). Fine for a game's
  assets today; `kpak_write` already writes through a callback, so a streaming write
  is a change here, not in the format.
- **Repack is deterministic.** Packing a directory and repacking a zip of the same files
  give the same bytes.
- **kpack is always the host's.** On `linux-dev` it is `bin/kpack`; in any other build
  (`linux-ship`, `linux-debug`, a cross build) kormake builds a host copy at
  `host/bin/kpack`, in the host config, and the pack rule runs that one.

## Not in this module

Compression, cooking (a cooker is another tool whose outputs are staged), incremental
packs, and patch packs (mount a second pack over the first instead).
