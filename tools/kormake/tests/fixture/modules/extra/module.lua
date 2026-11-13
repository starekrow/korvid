-- Outside the roots, on a package that is not there: left out, with why.
module "extra" {
  deps     = { "base" },
  external = { missing = ">=1" },
}
