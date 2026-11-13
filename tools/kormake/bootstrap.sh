#!/bin/sh
# Builds kormake from source with the host C compiler: the engine's vendored Lua (the lua
# module's, modules/lua/vendor/lua), the embedded scripts, and the host. Output:
# tools/kormake/bin/<host>/kormake. Run from anywhere.
set -e
here=$(cd "$(dirname "$0")" && pwd)
host=$(uname -s | tr '[:upper:]' '[:lower:]')
case "$host" in darwin) host=macos;; esac
out="$here/bin/$host"
tmp="$here/bin/.build"
lua="$here/../../modules/lua/vendor/lua"
cc="${CC:-cc}"
mkdir -p "$out" "$tmp"
echo "kormake: building for $host with $cc"
"$cc" -O2 -o "$tmp/embed" "$here/src/embed.c"
"$tmp/embed" "$tmp/scripts.inc" "$here"/scripts/*.lua
luasrc=""
for f in "$lua"/*.c; do
  case "$f" in */lua.c|*/luac.c) ;; *) luasrc="$luasrc $f";; esac
done
"$cc" -O2 -std=gnu99 -DLUA_USE_POSIX -I"$lua" -I"$tmp" -o "$out/kormake" "$here/src/kormake.c" "$here/src/os_posix.c" $luasrc -lm
echo "kormake: $out/kormake"
