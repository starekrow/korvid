# Contributing to Korvid

## Building and testing

The README's Quick Start and Platform Notes cover the toolchain for each platform. Before
a change goes in, it builds and its tests pass on every platform it touches:
`kormake test <target>` (on Windows, from Git Bash: the tests run shell scripts). Warnings
are errors everywhere (`-Werror`, `/WX`), and the compilers do not warn about the same
things, so a change that builds clean on one platform can still break another.

## Commits

A subject that says what changed, in the module's words (`platform_hid: ...`,
`goldberg: ...`), and a body that says why, and what was measured or found on the way.

### AI assistance: `Assisted-by`

A commit made with an AI assistant ends with an `Assisted-by` trailer naming the model:

```
Assisted-by: Claude Opus 5.5 <noreply@anthropic.com>
```

**Not `Co-Authored-By`.** The person who commits is the author and answers for the change;
the trailer records that a tool helped. Some assistants add `Co-Authored-By` on their own:
replace it before the commit is pushed.
