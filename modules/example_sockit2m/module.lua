-- An I/O torture test, not a game: no window, no engine. It pushes the I/O queue (files
-- through io_uring and the synchronous path) and the non-blocking sockets as hard as one
-- thread can, alone and together, and prints what it saw in numbers.
module "example_sockit2m" {
  output = "exe",
  deps   = { "core", "platform" },
}
