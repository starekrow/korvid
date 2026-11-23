-- The engine's base: types, memory, handles, the schedule, events, and the thread layer.
-- On Windows the thread layer asks MMCSS for the realtime audio class (avrt).
module "core" {
  external = { avrt = { ">=1", platforms = { "windows" } } },
}
