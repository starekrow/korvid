-- Image files: PNG (decoded to RGBA8 and encoded from RGB or RGBA, on the inflate module's
-- zlib) and binary PPM. Plain functions over memory: no engine state, no I/O, so the
-- asset module and a host tool use the same code.
module "image" {
  deps = { "core", "inflate" },
}
