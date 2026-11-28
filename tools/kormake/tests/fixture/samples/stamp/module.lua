-- A host tool on a private path that publishes two commands: a project beside the fixture
-- can name it and run it, and cannot build on it.
module "stamp" {
  output    = "exe",
  platforms = { "host" },
  publish   = { "stamp", "stamper" },
}
