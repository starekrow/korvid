-- goldberg's five screens in Krome (docs/PRDs/Krome-Slice-4-PRD.md, KW15): inspection, diff
-- review, live status, reports and forms, on sample models in the shapes goldberg has, out of
-- krome/std's widgets. --demo drives each by pointer and keyboard and checks the model.
module "kromescreens" {
  output        = "exe",
  engine_module = true,
  deps          = { "core", "platform", "platform_hid", "math", "render", "asset", "asset_live", "krome", "frameloop" },
}
