-- Inner engine: audio. A mixer on the device's thread fed by a lock-free ring; voices
-- with volume, pitch, pan and loop over sound assets; buses; voices that pause with the
-- sim. A machine without a device runs silent.
module "audio" {
  deps          = { "core", "math", "platform", "asset" },
  engine_module = true,
}
