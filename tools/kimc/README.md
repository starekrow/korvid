# kimc

The command line over the `kim` module, `bin/kimc`: `.kim` text to `.kimb`. An engine tool
like kbor (see `modules/tool`): its verbs are CLI commands, its work a system over the I/O
queue. Built for the host: `core`, `platform`, `cli`, `tool`, `kim`.

## Using it

```bash
kimc compile ui/form.kim ui/form.kimb                     # the document records "ui/form.kim"
kimc compile src/form.kim out/form.kimb --as game/form.kim # the asset path it records
kimc compile ui/form.kim ui/form.kimb --no-lines          # without the lines sidecar
kbor decode ui/form.kimb                                  # what it holds, as JSON
```

The exit status is 0 on success, 1 when the job failed (a file that would not read or
compile, a write that failed), 2 for a command line the CLI refused. Errors go to stderr
one to a line, `file:line:col: text`, so an editor can jump to them. The output is written
to a temporary name beside it and renamed into place, so it is never half written, and a
failed compile leaves no output.
