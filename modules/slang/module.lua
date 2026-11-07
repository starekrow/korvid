-- The shader compiler as a module: it provides "shader-compile" and exports the rule
-- that turns a .slang file into SPIR-V. A system lookup for now; a vendored binary would
-- be prebuilt = { linux = "bin/linux/slangc" } in this directory.
module "slang" {
  output   = "exe",
  provides = "shader-compile",
  system   = "slangc",
  exports  = {
    rule "shader" {
      inputs  = "shaders/*.slang",
      outputs = "%{stem}.spv",
      command = "%{tool} %{in} -target spirv -fvk-use-entrypoint-name -depfile %{depfile} -o %{out}",
      deps    = "depfile",
    },
  },
}
