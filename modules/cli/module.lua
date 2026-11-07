-- The CLI: a line protocol over stdin and TCP, the commands every module declared, and
-- request ids so a tool can multiplex. Reads run on the main thread at the start of the
-- tick against the last one; a write reaches its target before the next frame.
module "cli" {
  deps          = { "core", "platform" },
  engine_module = true,
}
