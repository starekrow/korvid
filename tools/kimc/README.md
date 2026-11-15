# kimc

The command line over the `kim` and `kimb` modules, `bin/kimc`: `.kim` text to `.kimb`, and
a file checked against its preludes by the rules Krome loads it by. An engine tool like
kbor (see `modules/tool`): its verbs are CLI commands, its work a system over the I/O
queue. Built for the host: `core`, `platform`, `cli`, `tool`, `kimb`, `kim`.

## Using it

```bash
kimc compile ui/form.kim ui/form.kimb                     # the document records "ui/form.kim"
kimc compile src/form.kim out/form.kimb --as game/form.kim # the asset path it records
kimc compile ui/form.kim ui/form.kimb --no-lines          # without the lines sidecar
kbor decode ui/form.kimb                                  # what it holds, as JSON
kimc check ui/form.kim --root krome=modules/krome/assets  # every mistake, against krome/std
kimc check ui/form.kim --tree build/assets/mygame         # preludes from a cooked tree
```

**check** reads the file and every prelude it uses, as they are found: a use `NAME/x` is
`x.kim` or `x.kimb` under the directory a `--root NAME=DIR` gives, or `NAME/x.kimb` (or
`.kim`) under a `--tree DIR`. It reports what `kimb_check` finds (`modules/kimb`): words
that are neither the kernel's nor a definition, styles and tokens that are not there,
properties a node does not take, values of the wrong type, a definition used with what it
does not take. A handful of small files: read in place, not queued.

The exit status is 0 on success, 1 when the job failed (a file that would not read or
compile, a write that failed), 2 for a command line the CLI refused. Errors go to stderr
one to a line, `file:line:col: text`, so an editor can jump to them. The output is written
to a temporary name beside it and renamed into place, so it is never half written, and a
failed compile leaves no output.
