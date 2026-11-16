#!/bin/sh
# The rule that lets fast code live beside the sim (docs/PRDs/CPU-Renderer-PRD.md, "Fast code
# where it pays", rule 2): an object built with a fast code-generation profile shares no
# inline function with the rest of the program. An inline function is a weak definition the
# linker may take from any object that has one; taken from a fast object, the sim would run
# on code compiled with contracted multiply-adds. This lists the weak symbols each fast object
# defines and fails if any other object of the build defines the same.
#
#   tools/check-fast-objects.sh [build/linux-dev]
set -e
export LC_ALL=C # sort and comm agree on the order
build=${1:-build/linux-dev}
# The fast objects: renderer-cpu-raster's (the kernels, every instruction set's copy), and
# every game shader's generated C++ (slang's shader_cpu rule: gen_<shader>_shader_cpu.o),
# which is full of the Slang prelude's inline math.
fast="$(find "$build/obj/renderer_cpu_raster" -name '*.o' | sort | tr '\n' ' ') $(find "$build/obj" -name 'gen_*_shader_cpu.o' | sort | tr '\n' ' ')"
for f in $fast; do [ -f "$f" ] || { echo "check-fast-objects: no $f (build first)"; exit 2; }; done
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
for f in $fast; do nm --defined-only "$f" | awk '$2 ~ /^[WVu]$/ {print $3}'; done | sort -u > "$tmp/fast"
others=$(find "$build/obj" -name '*.o' | grep -v -F "$(echo $fast | tr ' ' '\n')")
for o in $others; do nm --defined-only "$o" 2>/dev/null | awk '$2 ~ /^[WVuTt]$/ {print $3}'; done | sort -u > "$tmp/others"
shared=$(comm -12 "$tmp/fast" "$tmp/others")
n=$(wc -l < "$tmp/fast")
if [ -n "$shared" ]; then
  echo "check-fast-objects: FAIL: fast objects define weak symbols other objects also define:"
  echo "$shared" | c++filt | sed 's/^/  /'
  exit 1
fi
echo "check-fast-objects: ok ($n weak symbols in the fast objects, none shared)"
