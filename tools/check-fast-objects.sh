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

# cl's objects (.obj): an inline function is a COMDAT section, and the linker keeps the
# first copy it meets, which may be a fast object's: the same hazard, under another name.
# /O2 makes every function a COMDAT (function-level linking), so being one says nothing by
# itself; what fails is a fast object's COMDAT that another object also defines. dumpbin is
# the one beside the build's link.exe.
if [ -d "$build/obj/renderer_cpu_raster" ] && ls "$build/obj/renderer_cpu_raster"/*.obj >/dev/null 2>&1; then
  link=$(sed -n 's/^link *= *"\{0,1\}\([^"]*\)"\{0,1\} *$/\1/p' "$build/build.ninja" | head -1)
  dumpbin="${link%/*}/dumpbin.exe"
  [ -x "$dumpbin" ] || { echo "check-fast-objects: no dumpbin beside $link"; exit 2; }
  tmp=$(mktemp -d)
  trap 'rm -rf "$tmp"' EXIT
  find "$build/obj/renderer_cpu_raster" -name '*.obj' ! -path '*/tests/*' > "$tmp/fast.list"
  find "$build/obj" -name 'gen_*_shader_cpu.obj' >> "$tmp/fast.list"
  sort -o "$tmp/fast.list" "$tmp/fast.list"
  find "$build/obj" -name '*.obj' | sort | comm -23 - "$tmp/fast.list" > "$tmp/others.list"
  # Functions only (a symbol of type "()"): the rule is about code. cl pools string literals
  # (??_C@) and a CRT header's statics across objects, which are data and the same bytes
  # everywhere; its own helpers (??_H and ??_I, the array constructor and destructor loops)
  # hold no arithmetic. Nor do the UCRT's stdio functions, which its <stdio.h> defines
  # inline (glibc's are the library's): select.cc's fprintf is one. Options with '-': Git
  # Bash would take '/symbols' for a path.
  functions() {
    tr '\n' '\0' < "$1" | xargs -0 -n 64 "$dumpbin" -nologo -symbols \
      | awk '/External/ && !/UNDEF/ && /\(\)/ { for (i = 1; i <= NF; ++i) if ($i == "|") { print $(i + 1); break } }' \
      | grep -v -E '^\?\?_[HI]@|^_?_*(local_stdio_[a-z_]+_options|v?[sfn]?printf(_s)?|v?s?n?printf|_v?sn?printf(_s)?)$' | sort -u
  }
  functions "$tmp/fast.list" > "$tmp/fast"
  functions "$tmp/others.list" > "$tmp/others"
  shared=$(comm -12 "$tmp/fast" "$tmp/others")
  n=$(wc -l < "$tmp/fast")
  if [ -n "$shared" ]; then
    echo "check-fast-objects: FAIL: functions in the fast objects that other objects also define:"
    echo "$shared" | sed 's/^/  /'
    exit 1
  fi
  echo "check-fast-objects: ok ($n functions in the fast objects, none shared)"
  exit 0
fi

# The fast objects: renderer-cpu-raster's (the kernels, every instruction set's copy; not its
# test, which is built as the rest are), and
# every game shader's generated C++ (slang's shader_cpu rule: gen_<shader>_shader_cpu.o),
# which is full of the Slang prelude's inline math.
fast="$(find "$build/obj/renderer_cpu_raster" -name '*.o' ! -path '*/tests/*' | sort | tr '\n' ' ') $(find "$build/obj" -name 'gen_*_shader_cpu.o' | sort | tr '\n' ' ')"
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
