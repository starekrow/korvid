#!/bin/sh
# Runs one test executable inside the Android emulator (or on a connected device): kormake's
# test runner for the `androidemu` and `android` platforms (toolchain-android's test_runner),
# as `xcrun simctl spawn booted` is the iOS Simulator's. A test is an ordinary executable
# against bionic, so it runs from adb's shell with no package: pushed to
# /data/local/tmp/korvid/<target>/bin/ with what it reads (its cooked assets, shaders/),
# and run from the directory above, as kormake runs a test from the build directory here.
#
#   emu_test.sh /abs/path/to/build/<target>/bin/<suite>_tests
#
# From the environment:
#   ANDROID_SERIAL  the device, when adb shows more than one (adb's own variable)
#   ANDROID_HOME    where adb is, when it is not on PATH (else ~/Android/Sdk)
set -eu

exe=${1:?usage: emu_test.sh EXE}
name=$(basename "$exe")
bin=$(dirname "$exe")
target=$(basename "$(dirname "$bin")")

adb=$(command -v adb || true)
if [ -z "$adb" ]; then
  adb="${ANDROID_HOME:-$HOME/Android/Sdk}/platform-tools/adb"
  [ -x "$adb" ] || { echo "emu_test: no adb on PATH or under ANDROID_HOME (sdkmanager \"platform-tools\")" >&2; exit 2; }
fi
state=$("$adb" get-state 2>&1 || true)
if [ "$state" != device ]; then
  echo "emu_test: no one device to run on ($state). Start the emulator (emulator -avd korvid -gpu host)," >&2
  echo "emu_test: or set ANDROID_SERIAL when \`adb devices\` shows several." >&2
  exit 2
fi

# The executable must be the device's own architecture: the emulator runs androidemu's
# x86-64, a phone android's arm64. Said here, since the shell's own answer is "not executable".
abi=$("$adb" shell getprop ro.product.cpu.abi | tr -d '\r')
case "$target" in
  androidemu-*) want=x86_64 ;;
  android-*)    want=arm64-v8a ;;
  *)            want=$abi ;;
esac
if [ "$abi" != "$want" ]; then
  echo "emu_test: $target builds for $want and the device is $abi" >&2
  exit 2
fi

dir=/data/local/tmp/korvid/$target
"$adb" shell "mkdir -p $dir/bin $dir/tmp"
# What the tests read beside them. Shaders are the build's, once for all of its suites.
# Assets are the tree goldberg cooked for this suite, named by <suite>.assets beside it:
# its modules' folders go into assets/ on the device, where every suite's land together
# (a module's cooked files are the same whichever suite brought them). adb's sync sends
# only what changed, and the lock keeps suites that start together from sending it twice.
if [ -d "$bin/shaders" ]; then
  ( flock 9; "$adb" push --sync "$bin/shaders" "$dir/bin/" >/dev/null 2>&1 ) 9>"$bin/.emu_test.shaders.lock" ||
    { echo "emu_test: could not push shaders/" >&2; exit 2; }
fi
if [ -f "$bin/$name.assets" ]; then
  tree=$(head -n 1 "$bin/$name.assets" | tr -d '\r')
  if [ -d "$tree" ]; then
    "$adb" shell "mkdir -p $dir/bin/assets"
    for m in "$tree"/*/; do
      [ -d "$m" ] || continue
      ( flock 9; "$adb" push --sync "${m%/}" "$dir/bin/assets/" >/dev/null 2>&1 ) 9>"$bin/.emu_test.assets.lock" ||
        { echo "emu_test: could not push $(basename "$m")/ of $tree" >&2; exit 2; }
    done
  fi
fi
# What it reads of the source tree (module.lua's test_files), named by <suite>.files beside
# it, the root on its first line: into bin/source/, beside the executable, where
# test_source_path looks on Android. Suites that list the same file send it once (the lock).
if [ -f "$bin/$name.files" ]; then
  root=$(head -n 1 "$bin/$name.files" | tr -d '\r')
  tail -n +2 "$bin/$name.files" | tr -d '\r' | while IFS= read -r rel; do
    [ -n "$rel" ] || continue
    [ -e "$root/$rel" ] || { echo "emu_test: $name.files names $rel, which is not in $root" >&2; exit 2; }
    to=$dir/bin/source/$(dirname "$rel")
    ( flock 9; "$adb" shell -n "mkdir -p $to" && "$adb" push --sync "$root/$rel" "$to/" >/dev/null 2>&1 ) </dev/null 9>"$bin/.emu_test.source.lock" ||
      { echo "emu_test: could not push $rel" >&2; exit 2; }
  done || exit 2
fi
"$adb" push "$exe" "$dir/bin/$name" >/dev/null 2>&1 || { echo "emu_test: could not push $name" >&2; exit 2; }

log=$(mktemp "${TMPDIR:-/tmp}/emu_test.XXXXXX")
trap 'rm -f "$log"' EXIT
set +e
"$adb" shell "cd $dir && TMPDIR=$dir/tmp ./bin/$name; echo \"emu_test: exit \$?\"" 2>&1 | tr -d '\r' | tee "$log" | grep -v '^emu_test: exit '
set -e
code=$(sed -n 's/^emu_test: exit \([0-9]*\)$/\1/p' "$log" | tail -1)
if [ -z "$code" ]; then
  echo "emu_test: $name did not report an exit code (the device went away)" >&2
  exit 1
fi
# A pass is the exit code and the suite's own summary (test_report's "<suite>: N checks
# passed") together, as on an iPhone: a crash must never read as a pass.
if [ "$code" = 0 ] && ! grep -q -E "test: $name: [0-9]+ checks passed" "$log"; then
  echo "emu_test: $name exited 0 without its summary (it crashed, or its output was cut off)" >&2
  exit 1
fi
exit "$code"
