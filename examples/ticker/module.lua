-- A window whose sim follows the clock and whose screen does not change: the measure of
-- Pacing::Decoupled (KR20). With --want-ticks it asks for every tick; without, it asks for
-- nothing. The frame loop's last line says how many frames it drew and ticks it ran.
module "ticker" {
  output        = "exe",
  engine_module = true,
  deps          = { "core", "platform", "platform_hid", "frameloop" },
}
