#!/bin/sh
# Builds and runs check.cc (see there): kv's math against MPFR. Linux only; it links the
# libmpfr.so.6 that GCC depends on, so nothing needs installing. With a function's name,
# only that one.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$here/../../..
out=${TMPDIR:-/tmp}/kv_math_check
mpfr=$(ldconfig -p | sed -n 's/.*libmpfr\.so\.6 .*=> //p' | head -1)
[ -n "$mpfr" ] || { echo "check: no libmpfr.so.6" >&2; exit 1; }
# The engine's flags for the math (toolchain-linux's), so what is checked is what ships.
g++ -std=c++20 -O2 -ffp-contract=off -fno-math-errno -fno-exceptions -fno-rtti -I"$root/modules/core/include" -I"$root/modules/math/include" \
  "$here/check.cc" "$root"/modules/math/src/*.cc "$mpfr" -lm -pthread -o "$out"
exec "$out" "$@"
