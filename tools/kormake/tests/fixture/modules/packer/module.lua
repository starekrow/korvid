-- A host tool with a rule that runs once per executable, over its staged assets. It
-- publishes its command (kormake link tools), which a project's module takes over.
module "packer" {
  output    = "exe",
  platforms = { "host" },
  publish   = true,
  exports   = {
    rule "pack" { per = "exe", inputs = "assets", outputs = "%{exe}.pak", configs = { "ship" }, command = "%{tool} %{out} @%{rsp}" },
  },
}
