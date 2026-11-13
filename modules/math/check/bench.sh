#!/bin/sh
# Builds and runs bench.cc: kv's math against glibc's, ns per call. Linux.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$here/../../..
out=${TMPDIR:-/tmp}/kv_math_bench
g++ -std=c++20 -O2 -ffp-contract=off -fno-math-errno -fno-exceptions -fno-rtti -I"$root/modules/core/include" -I"$root/modules/math/include" \
  "$here/bench.cc" "$root"/modules/math/src/*.cc -lm -o "$out"
exec "$out"
