-- A host tool with a rule that runs once per executable, over its staged assets.
module "packer" {
  output    = "exe",
  platforms = { "host" },
  exports   = {
    rule "pack" { per = "exe", inputs = "assets", outputs = "%{exe}.pak", configs = { "ship" }, command = "%{tool} %{out} @%{rsp}" },
  },
}
