-- kormake's test fixture: a small engine root that reaches every kind of edge kormake
-- writes. tests/run.sh generates it for a few targets and compares the result with
-- tests/expected/. Nothing here is compiled; the toolchains' commands are never run.
project "fixture"
requires_kormake "1.1"
modulepath { "modules" }
modulepath { "samples", private = true }
platforms { "linux", "macos", "windows", "cross" }
configs   { "dev", "ship" }
root "all" { modules = { "base", "gfx", "game" }, tests = true }
-- No goldberg here: assets are copied, a root at a time (copy_assets.sh, .bat on Windows).
cook "copy_assets {in} {out}"
