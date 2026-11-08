-- Deflate decompression: miniz's tinfl (MIT), vendored and pinned, behind a Korvid API.
-- Raw deflate (zip entries), zlib and gzip streams into caller buffers, CRC-32. No
-- allocation of its own; no zlib on the system.
module "inflate" {
  deps = { "core" },
}
