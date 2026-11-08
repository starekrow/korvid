-- The zip format as packs use it: reading a central directory and the .kpak index from
-- bytes the caller fetched, normalizing and hashing paths, and writing aligned, indexed,
-- deterministic archives. No I/O of its own: the platform's mounts and kpack both use it.
module "pack" {
  deps = { "core", "inflate" },
}
