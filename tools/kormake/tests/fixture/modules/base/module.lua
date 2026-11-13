-- A library with two sources (a unity file), a platform source directory, a package with
-- a runtime file, tests, and assets, one of whose names Ninja has to escape.
module "base" {
  external = { pkg = ">=1", sys = { ">=1", platforms = { "cross" } } },
  defines  = { "BASE=1" },
}
