-- The shader compiler as a module: it provides "shader-compile" and exports the rule
-- that turns a .slang file into what the platform's renderer loads, with every module's
-- shaders/ directory on the include path (a game's shader includes the render interface's
-- sprite_fragment.slangh by name). A system lookup for now; a vendored binary would be
-- prebuilt = { linux = "bin/linux/slangc" } in this directory.
--
-- SPIR-V for Vulkan by default. On the Apple family a Metal library: Slang drives Xcode's Metal compiler
-- itself (the Metal Toolchain component must be installed), defines KV_TARGET_METAL for the
-- render interface's Metal-only declarations, and writes its reflection beside the library
-- (<out>.json: every parameter's binding by name), which a dev build of renderer-metal checks
-- its layouts against. docs/design/renderer-metal.md has why.
module "slang" {
  output   = "exe",
  provides = "shader-compile",
  system   = "slangc",
  exports  = {
    rule "shader" {
      inputs  = "shaders/*.slang",
      outputs = "%{stem}.spv",
      command = "%{tool} %{in} %{shader_includes} -target spirv -fvk-use-entrypoint-name -depfile %{depfile} -o %{out}",
      deps    = "depfile",
      platforms = {
        -- The Apple family (toolchain.apple's family): Metal's compiler builds for the
        -- consuming platform's triple (air64-apple-ios26.0-simulator), since a library is
        -- built for one OS and an iOS Metal refuses a macOS library.
        apple = {
          outputs = "%{stem}.metallib",
          command = "%{tool} %{in} %{shader_includes} -target metallib -DKV_TARGET_METAL=1 -Xmetal -target -Xmetal air64-%{triple} " ..
                    "-depfile %{depfile} -reflection-json %{out}.json -o %{out}",
        },
      },
    },
  },
}
