-- Outer engine: assets. load_texture returns a handle at once; the file read goes
-- through the platform I/O queue, the decode and upload happen in "asset.pump" (phase
-- PreSim), and completion arrives as AssetReady or AssetFailed on the bus, never inline.
module "asset" {
  deps     = { "core", "platform", "math", "render" },
  external = { zlib = ">=1.2" },
  engine_module = true,
}
