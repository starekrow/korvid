-- An I/O torture test, not a game: no window. A module whose systems push the I/O queue
-- (files through io_uring and the synchronous path) and the non-blocking sockets as hard
-- as the engine's own path allows: one pump per frame, socket handling in a tick system,
-- completions reaped by a main-thread system. The same scenarios also run as a tight loop
-- over the primitives (--direct), for comparison. It prints what it saw in numbers.
module "example_sockit2m" {
  output        = "exe",
  deps          = { "core", "platform" },
  engine_module = true,
}
