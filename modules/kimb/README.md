# kimb

The `.kimb` document and what its names mean, without Krome. `kim` (the parser, the
compiler) and Krome (the loader) both link it, so a mistake is found by the same rules in
`kimc check`, goldberg's cook and the running game (`docs/PRDs/Krome-Slice-2-PRD.md`,
"Meaning"). Depends on `core` only.

## How it works

**Reading.** `kimb_read` is `kbor_parse` and a look at the shape: the version (1), the
sections (`uses`, `scripts`, `props`, `screens`, `defs`, `styles`, `themes`), and the lines
sidecar moved onto the values (`JsonValue::line`) in the walk `korvid/kim/kim.h` gives, so
whoever reports a mistake asks the value where it was written. `kimb_from_doc` does the
same for a document already in memory (`kim_parse`'s).

**The scope.** A file's names resolve in the file, then each `use` in the order written
(and theirs, depth first, each file once); `kimb_scope` lays that out once the files are
found. Definitions, styles and `prop` declarations are the first file's that has one. A
token is looked up in the theme of that name in the same order, so a game's `theme dark`
can override std's tokens one by one and keep the rest.

**The kernel's words.** Its four nodes, its flags, its states, and its properties with
their types (`kimb_kernel_prop`), which are the tree's (`modules/krome/src/tree.cc`): a
fixed vocabulary that a prelude does not declare again.

**Values.** `kimb_resolve_value` gives what a property takes in a theme: a token of the
theme first (a word a theme defines wins over a literal spelled the same), then a literal
of the property's type.

**The check.** `kimb_check` walks the first file's screens, definitions and styles: words
that are neither the kernel's nor a definition, styles and tokens that are not there (a
token must be in every theme the scope has, since a theme can be switched to), properties
a node does not take and values of the wrong type, a definition given more strings than it
has parameters, a property it does not take, or children it has no slot for; looks that
set layout, states that are not, styles that start from themselves. In a sparse `each`
(one with `count=` or `from=`) a child `placeholder { ... }` is the block for rows the model
does not hold: its children are checked as the row's are, and it takes nothing else. A
second one, or a `placeholder` anywhere else, is a mistake, unless a definition of that
name is in scope outside a sparse `each`.

## Using it

```cpp
KimbFile f;
KimErrors errors;
if (!kimb_read(bytes, size, &arena.a, f, errors, "game/form.kimb")) ...
KimbScope scope;
kimb_scope(scope, f, find_use, user, errors); // find_use("krome/std") gives that file, or null
kimb_check(scope, errors);
for (u32 i = 0; i < errors.count; ++i) { char line[300]; errors.format_one(i, line, sizeof line); ... }
```

## Behaviour worth knowing

- **Errors are capped.** `KimErrors` keeps 32, with the file, line and column (and, for
  a mistake found through a definition, where it was used), and counts the rest.
- **A stripped file has no lines.** Its values say line 0.
