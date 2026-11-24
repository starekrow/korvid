-- An executable on an engine module, a package pkg-config finds (the fake one in pkgs/,
-- with explicit fields for where there is none), and the config's directory external.
module "player" {
  output    = "exe",
  deps      = { "base", "codec" },
  external  = { fakepw = ">=0.3", shared = ">=1" },
  externals = {
    linux = { fakepw = { pkg_config = "libfakepw-0.3", libs = { "fakepw-fallback" }, fetch = "sudo apt install libfakepw-0.3-dev" } },
    macos = { fakepw = { pkg_config = "libfakepw-0.3", libs = { "fakepw-fallback" }, fetch = "brew install fakepw" } },
    windows = { fakepw = { pkg_config = "libfakepw-0.3", libs = { "fakepw-fallback" }, fetch = "vcpkg install fakepw" } },
  },
}
