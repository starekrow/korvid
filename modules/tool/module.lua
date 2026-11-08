-- Tools built from the engine: tool_main starts a headless engine with the tool's module
-- list, runs the command line as one CLI request, and ticks until a system says the work
-- is done. A tool is a module whose verbs are CLI commands and whose work is systems.
module "tool" {
  deps          = { "core", "platform", "cli" },
  engine_module = true,
}
