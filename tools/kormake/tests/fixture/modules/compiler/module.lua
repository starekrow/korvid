-- A tool built for the host, from a library the target builds too, with tests of its own.
module "compiler" {
  output    = "exe",
  platforms = { "host" },
  deps      = { "base" },
  exports   = {
    rule "compile" { inputs = "shaders/*.shd", outputs = "%{stem}.bin", command = "%{tool} %{in} -o %{out}" },
  },
}
