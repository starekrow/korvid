-- BulletML: patterns as documents (read through the xml module), compiled once into a
-- library of ops and expressions, run by plain-data runners a game steps every tick. A
-- library: no ktables, no systems; the game owns its bullets.
module "bulletml" {
  deps = { "core", "math", "xml" },
}
