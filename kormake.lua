-- Korvid master file. Read top to bottom.
-- Not executed yet: kormake does not exist. build.ninja is the hand-written
-- stand-in and is deleted the day kormake emits it.

project "korvid"
requires_kormake "1.0"

modulepath { "modules" }

platforms { "linux" }
configs   { "debug", "dev", "ship" }

-- platform modules will supply this default; until then it is spelled out
bind "render" { linux = { "renderer-vulkan" } }

root "example_testbed" {}
root "tests" { modules = "*" }
