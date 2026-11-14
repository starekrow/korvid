-- A tool built for the host, from a library the target builds too, with tests of its own.
-- Its rule has a variant for wherever gfx-soft is bound (as slang's DXIL form follows
-- renderer-d3d12): it wins over the platform's own variant, and names the Ninja rule.
module "compiler" {
  output    = "exe",
  platforms = { "host" },
  deps      = { "base" },
  exports   = {
    rule "compile" {
      inputs = "shaders/*.shd", outputs = "%{stem}.bin", command = "%{tool} %{in} -o %{out}",
      platforms = {
        cross        = { command = "%{tool} %{in} --cross -o %{out}" },
        ["gfx-soft"] = { outputs = "%{stem}.soft", command = "%{tool} %{in} --soft -o %{out}" },
      },
    },
  },
}
