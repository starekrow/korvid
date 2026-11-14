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
