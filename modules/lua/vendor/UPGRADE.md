# Vendored Lua

`lua/` is the `src/` directory and `README` of the official Lua 5.5.1 distribution,
unchanged. The lua module compiles it for the engine (`src/lua.cc`), and kormake's
bootstrap compiles it into kormake: one copy for both. MIT license, in `lua/README`.

    lua-5.5.1.tar.gz  sha256 1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce

To upgrade:

1. Fetch the release tarball from https://www.lua.org/ftp/ and check its SHA-256 against
   the one published at https://www.lua.org/ftp/#hashes.
2. Replace `lua/` with the tarball's `src/` directory plus its top-level `README`. Leave
   out `doc/` and the `Makefile`; kormake's `bootstrap.sh` and `bootstrap.bat` compile
   every `lua/*.c` except `lua.c` and `luac.c`, the two standalone programs, and
   `src/lua.cc` names each of the others.
3. Update the version and hash above and in `tools/kormake/README.md`.
4. Read the manual's "Incompatibilities" section for the language changes and run
   `kormake check` on this repository, kormake's own test (`tools/kormake/tests/run.sh`)
   and the lua module's (`kormake test lua`). 5.5 made `global` a reserved word and loop
   control variables read-only; the scripts avoid both.

The tarball itself is not tracked (`*.tar.gz` is ignored in this directory).
