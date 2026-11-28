# kbor

The KBOR tool, `bin/kbor`: JSON to KBOR and back, and KBOR in its tightest form. The
format is `docs/design/kbor.md`; the reader and writer are core's (`korvid/core/kbor.h`),
so this is a command line over them. It is an engine tool like kpack and kvimagetool (see
`modules/tool`): its verbs are CLI commands, its work a system over the I/O queue. Built
for the host, linking no system library: `core`, `platform`, `cli`, `tool`.

## Using it

```bash
kbor encode level.json level.kbor          # JSON as normalized KBOR
kbor decode level.kbor                     # KBOR as JSON on stdout, pretty
kbor decode level.kbor level.json --compact
kbor normalize save.kbor                   # in place
kbor normalize save.kbor save.tight.kbor
kbor info save.kbor                        # normalized or not, and what it holds
kbor help
```

Engine flags go first: `kbor --verbose decode ...`. The exit status is 0 on success, 1
when the job failed (a file that would not read or parse, a write that failed), 2 for a
command line the CLI refused.

- **encode** reads JSON (`json_parse`) and writes normalized KBOR (`kbor_write`): definite
  lengths, an index for every repeated key, the summary in front.
- **decode** reads any KBOR and writes JSON (`json_write`) as "JSON and back" in the
  design has it: integers as digits, floats always with a fraction or an exponent, NaN and
  the infinities as `"nan"`, `"inf"`, `"-inf"`, byte strings as base64. Pretty by default:
  two spaces a level, an array of numbers on one line. `--compact` has no white space.
- **normalize** reads any KBOR (a streaming writer's, a foreign encoder's, one with
  indefinite lengths or names in full every time) and writes it normalized. Without an
  output it works in place; a file that is already normalized is left as it is.
- **info** says whether a file carries the summary, and counts its values, keys (text and
  index), distinct names, strings and string bytes.

## How it works

A command checks its words and records the job; `kbor.pump` (PreSim, main thread, every
tick) reads the input through the I/O queue, parses it into a tree in an arena sized from
the input, writes the output to `<out>.kbor-tmp` through the queue, and renames that over
the output once the write is done, so an output is never left half written, normalizing
in place included. Then a line goes to stdout (not for decode to stdout) and
`tool_finish` ends the run.

On a 53 MB JSON document of 3.9 million values (a dev build, Linux): encode 0.40 s to
19.8 MB, normalizing the same data from a stock encoder's 35 MB 0.27 s, info on the
normalized file (a full read) 0.08 s, decode 0.74 s.

## Quick reference

| name | kind | what |
|---|---|---|
| `kbor encode`, `kbor decode`, `kbor normalize`, `kbor info` | CLI commands | above |
| `kbor.pump` | system, phase PreSim, main thread, `Always` | the job |

Tests: `tests/main.cc` runs the built tool over files in a temporary directory. The
format's tests are core's.

## Not in this tool

Schemas, reading JSON from stdin or writing KBOR to stdout, and more than one file a run.
