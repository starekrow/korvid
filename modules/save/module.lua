-- Saving and loading: a structured writer (JSON today, room for CBOR), any schema'd row as
-- named fields, and whole ktables restored slot for slot, handles by kind name.
module "save" {
  deps = { "core", "math" },
}
