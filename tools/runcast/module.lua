-- runcast: a child process's output served over a loopback socket, and fetched. A small
-- tool that touches processes, sockets, the lock, the wait and the I/O queue at once: it
-- was written from the public headers alone, as a cold read of the platform layer, and is
-- kept as the check that a tool which only waits uses no CPU (tools/runcast/README.md).
module "runcast" {
  output        = "exe",
  platforms     = { "host" },
  deps          = { "core", "platform", "cli", "tool" },
  engine_module = true,
}
