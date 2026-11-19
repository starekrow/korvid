-- dejavu: DejaVu Sans, the engine's default font, as an asset (assets/, with its license).
-- No code: one header names its asset path. Krome, sprite_ui and frameloop fall back to it;
-- a game that wants it for its own text depends on this module too.
module "dejavu" {
  deps     = { "core" },
  licenses = { "assets/DejaVuSans-LICENSE.txt" },
}
