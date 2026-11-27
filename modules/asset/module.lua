-- Outer engine: assets. A load returns a handle at once; the file read goes through the
-- platform I/O queue, each kind's pump (phase PreSim) makes the record, and completion
-- arrives as AssetReady or AssetFailed on the bus, never inline. Blobs, shapes, fonts,
-- sounds and streams are here, with the scopes that hold every kind; nothing here draws, so
-- a tool links it without a renderer. Textures and materials are asset_render's.
module "asset" {
  deps     = { "core", "platform", "math", "truetype", "patherizer" },
  engine_module = true,
  test_files = { "modules/dejavu/assets/DejaVuSans.ttf", "examples/arcfire/assets/font/PublicPixel.ttf" }, -- the font test's two faces
}
