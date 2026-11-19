# Documentation style

How Korvid's documentation is laid out, what goes where, and how it is written. This is
for anyone who edits the docs, people and AI assistants alike. Read it before writing
docs; [tools/doccheck.sh](tools/doccheck.sh) checks the parts a script can check.

The aim: someone new to the engine can start at one page, find what they want to do,
follow it down to the exact function, and pick up hints along the way about what else
the engine can do. Someone who already knows the engine can go straight to a module page
or a header and find every detail there.

## The layers

Each kind of document answers one kind of question and lives in one place.

| layer | answers | where | tense and shape |
|---|---|---|---|
| Landing page | where do I start? | [docs/README.md](docs/README.md) | a reading path, then a map of everything |
| Tasks | how do I…? | [docs/tasks.md](docs/tasks.md) | one line per task, linking to a guide, a module and a symbol |
| Guides | walk me through doing X | `docs/guides/` | second person, built around one real example, ends with where to go next |
| Concepts | how does X work across the engine? | `docs/concepts/` | present tense, how it is now; no dates, no history |
| Module page | what is this module and how do I use it? | `modules/<m>/README.md` | the module template (below) |
| Reference | what exactly does this symbol do? | `modules/<m>/include/korvid/<m>/*.h` | comments on declarations (below) |
| Samples | show me working code | `examples/<e>/README.md` and their sources | what the example shows, and which modules it uses |
| Tools | how do I run this tool? | `tools/<t>/README.md` | usage first |
| Design records | why is it this way, and what was tried? | `docs/design/` | dated, written at the time; may describe the past |
| Plans | what was planned, slice by slice | `docs/PRDs/` | dated; history once built |

**Each fact has one home.** What a symbol means lives in its header. How a module's parts
fit together lives in its README. How a task crosses modules lives in a guide. Why a
design was chosen lives in its design record. Other documents link to that home rather
than restating it. A guide can say "`collision_query` lists the colliders in a
rectangle" in passing, but the rules for what it returns stay in the header.

**Links go down and sideways, never only up.** The landing page links to guides and
modules, guides link to concepts and modules, modules link to headers and examples. A
reader can always go one level deeper from where they are.

**The top-level README is the author's.** `README.md` at the root is written and kept
by the project's author. Docs work leaves it alone, except for its platform build
instructions. It links once to [docs/README.md](docs/README.md), and everything else
hangs from there.

## Hints

Readers find out what is possible when the docs point it out at the moment it is
relevant. Three places do this:

- **See also** at the end of a module page, concept page or guide: two to five
  neighbours, each with half a line saying why you would go there.
- **Seen in** on a module page: which examples use the module, and for what ("rocks:
  bullet expiry with `schedule_in`"). Readers get pointed at real code, and examples get
  found.
- **Where next** at the end of a guide: the two or three guides or concepts a reader is
  most likely to want after this one.

## Markdown rules

- **Relative links** between documents in the repository, so they work on GitHub and
  in an editor: `[collision](../../modules/collision/README.md)`. The link text is the
  thing's name, never "here".
- **Headers can't link**, so in a header comment write a path from the repository
  root in backticks: `docs/concepts/scheduling.md`.
- **Headings are anchors.** Other pages link to `#quick-reference` and
  `#behaviour-worth-knowing`, so template headings keep their exact names. Renaming a
  heading means fixing its links (the checker finds them).
- **Code in code font.** Every engine name, file, flag and string the engine reads goes
  in backticks: `Collider2`, `collision.resolve`, `--threads`, `"*.Velocity"`.
- **Tables for lookups, prose for explanations.** A quick reference, a command line or a
  list of modes is a table. How something works is paragraphs.
- **Text diagrams** in fenced blocks, as [Modules](docs/concepts/modules.md#how-code-flows-through-a-tick)
  draws a tick. They read the same in an editor, a terminal and on GitHub.
- **Wrap at about 90 columns**, as the existing docs do. Tables and long links may run
  over.

## The module page

`modules/<m>/README.md`. [collision](modules/collision/README.md) is the model. Sections
in this order, with these names:

```
# <module>                       the module's name, as in module.lua

<one paragraph>                  what it is for. The first sentence stands alone: the
                                 landing page's catalog uses it.

## How it works                  the mechanism: what runs when, in which phase, with what
## Using it                      the common tasks, each a bold lead-in and a short snippet
## Quick reference               every public name, per header; what the module registers
## Behaviour worth knowing       the surprises: limits, edge cases, what a newcomer gets wrong
## Not in this module            what people look for here that lives elsewhere, or is not built
## Seen in                       examples that use it, and for what
## See also                      neighbouring modules, concepts, guides
```

A module with a large surface can add sections between "Using it" and "Quick reference"
(asset has "Lifetimes" and "Names"). A module no game calls directly (a renderer
backend, a toolchain, a vendored library's wrapper) uses a short page: the opening
paragraph, "What it provides" (what it implements and for whom), "Platform notes" if any,
and "See also". The opening paragraph says that games don't call it.

### Quick reference

The quick reference lets a reader look a name up without opening the header. It is
checked, so it has a fixed shape:

```
Header `korvid/<m>/<file>.h`:

| name | kind | what |
|---|---|---|
| `collision_query(ctx, rect, layers, out)` | function | the colliders overlapping a rectangle |
```

- **One table per header**, headed by the header's include path. A header with three
  names or fewer may be one line of prose instead.
- **`name`**: the name in backticks, with its arguments' names (no types) for a function.
  Names that belong together share a row, separated by commas.
- **`kind`**, from this vocabulary: `record`, `enum`, `function(s)`, `method(s)`,
  `macro(s)`, `constant(s)`, `component`, `event`, `event type`, `kind`, `resource`,
  `system, phase X`, `handler, phase X`, `ModuleDesc`. Combine with commas when a row
  mixes kinds.
- **`what`**: under a line; for a record, its fields.
- **Then "What the module registers with the engine"**: its components, kinds, event
  types, resources, systems and handlers with their phases and sets, as the module's
  `ModuleDesc` declares them.

Every name in the quick reference must exist in the module's code, and every public
function and record in its headers should appear in it. `tools/doccheck.sh --module <m>`
lists both kinds of difference. When a header changes, the quick reference changes in
the same commit.

## Headers

Headers are the reference: what a reader sees while typing. The detail lives here, next
to the code it describes, so it changes with that code.

**The file comment** comes first, above `#pragma once`:

```cpp
// Korvid collision: the Collider2 component, bodies that move by intent, and the overlap
// events.
//
// <how the pieces in this header work together, in paragraphs>
```

The first line is `// Korvid <module>: <what this header provides>.` The paragraphs that
follow cover what this header's declarations do together. They don't repeat the module
README: when the full picture is there, end with the path:
`// modules/collision/README.md has the whole model.`

**Every public declaration has a comment directly above it**, starting with what the
declaration is or does, in a phrase or a sentence: "The component.", "A circle on `layer`
that meets `mask`: a sensor.", "Parses argv over `config`, runs the engine and the loop,
returns the process exit code." Then whichever of these apply, in this order:

1. **when** it may be called: a phase, the main thread only, before `engine_init`,
   outside the schedule;
2. **what it returns on failure**: null, false, a null handle, and whether it logs;
3. **who owns** what goes in and comes out, and how long a pointer stays valid;
4. **cost** where it matters: allocates or not, a lock, O(n) over what;
5. **determinism**: whether it is sim state, hashed, recorded, or cosmetic.

**Fields** get trailing comments, with units and defaults' meanings:
`f32 radius = 0.0f; // Circle: radius in world units (pixels, in the examples)`.
Enumerators get them too.

**Group a long header** with section rules that a quick-reference table can mirror:
`// ---- the Work cycle ----------------------------------------`.

**No tags.** There is no `@param` or `\brief`. The prose already says which argument
is which, by name, in backticks.

**Public means under `include/korvid/<m>/`.** Anything there is documented. Helpers a
reader should not call live in `src/`, or in a public header after a section rule that
starts `// ---- internal` (the pieces behind a template, say). The quick reference leaves
those out, and the checker does too, up to the next section rule.

## Guides

`docs/guides/<topic>.md`, named for the task or the subject ("frame-loop-and-scheduling").

- **Open with what the reader will be able to do** and what they should know first, in
  a few lines.
- **Build on one real example** from `examples/`, and quote its code. Snippets are short
  and cite their source by file and function (`examples/rocks/src/game.cc`, `move`), not
  by line number, which goes stale.
- **Explain the mechanism once, at the depth the task needs**, then link to the concept
  page or module page for the rest.
- **Show what the engine prints** (the schedule log, an init error) when that is how
  the reader will see the thing working or failing.
- **End with "Where next".**

## Concepts

`docs/concepts/<idea>.md`. Each page describes how one engine-wide idea works now:
present tense, no dates, no "used to". When it changes, the page changes. The design
record that led to it stays as written, and the concept page links to it for the why.

## Tasks

[docs/tasks.md](docs/tasks.md) is grouped by area. Each line is a task in the reader's
words, then where to go:

```
- **Detect when two things touch:** [collision](../modules/collision/README.md), `OverlapBegin`
```

The bold part is what the reader wants, in their words, not the engine's: "detect when two
things touch", not "OverlapBegin events". After it comes one link to the page that answers
the task (a guide if there is one, otherwise the module), and then the one or two symbols
to look up. A task the engine doesn't do yet can be listed with "not built" and a link
to where it is planned, so the reader learns that rather than searching.

## Voice

The module docs already have a voice. [collision](modules/collision/README.md) is the
reference. Match it:

- **Say what a thing is first**, then how it works, then the exceptions.
- **Concrete over general**: units, numbers, names, the actual flag. "At most 4 ticks a
  frame", not "a bounded number".
- **Present tense, active voice.** "The resolve moves every intent", not "intents will be
  moved".
- **Short paragraphs with bold lead-ins** when a section covers several things.
- **Name the failure** and what the engine says when it happens.
- **No filler**: no "simply", "just", "easily", "powerful", "seamless", "note that".
- **Examples from the repository**, not made up, wherever one exists.
- **British "behaviour"** in the template heading, since the existing pages use it.
  Otherwise either spelling is fine.

## Keeping it right

- **Docs change in the same commit as the code they describe.** A changed signature
  updates its header comment and its quick-reference row. A new system updates "What the
  module registers". A new flag updates the command-line table.
- **Run the checker** before a docs commit, and over everything now and then:

  ```bash
  tools/doccheck.sh            # links in every document, quick-reference names
  tools/doccheck.sh --module frameloop   # also: public names missing from the quick reference
  ```

- **For AI assistants:** read this file and the page you are changing before you edit.
  Build a quick reference from the header as it is now, not from memory or from another
  page. Never edit the top-level `README.md` beyond its platform build instructions. When
  a fact has a home elsewhere, link to it.
