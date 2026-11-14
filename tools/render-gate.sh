#!/usr/bin/env bash
# The render gate (docs/PRDs/Engine-Slice-9-PRD.md, R9 and R10): what must hold on a
# backend's own machine before and after each milestone of the render core's extraction.
# Nothing here knows which milestone it is: it builds, draws, and compares.
#
#   tools/render-gate.sh linux   [vulkan]     Linux, renderer-vulkan
#   tools/render-gate.sh macos   [metal]      macOS, renderer-metal
#   tools/render-gate.sh windows [d3d12]      Windows under Git Bash; the backend is whichever
#                                             kormake.lua binds, named here for the references
#   tools/render-gate.sh <platform> <backend> --baseline
#       the same, at the commit before the slice (git checkout main): keeps the shaders'
#       hashes and the bench numbers under build/render-gate/<platform>-<backend>/ for the
#       later runs to be read against, and, where the backend has no committed gallery
#       references yet (d3d12), prints the command that makes them.
#
# Exit status 0: every check passed. The bench numbers are printed, not judged: read them
# against the baseline's (bench.txt beside the hashes), a few runs each.
set -u
cd "$(dirname "$0")/.."

platform=${1:?platform: linux, macos or windows}
case $platform in
    linux)   backend=${2:-vulkan}; exe= ;;
    macos)   backend=${2:-metal};  exe= ;;
    windows) backend=${2:-d3d12};  exe=.exe ;;
    *) echo "render-gate: unknown platform '$platform'"; exit 2 ;;
esac
baseline=0
for a in "$@"; do [ "$a" = "--baseline" ] && baseline=1; done

kormake=tools/kormake/bin/$platform/kormake$exe
dev=build/$platform-dev/bin
debug=build/$platform-debug/bin
keep=build/render-gate/$platform-$backend
out=$keep/run
refs=examples/gallery/references/$backend
mkdir -p "$out"
rm -f "$out"/*.ppm "$out"/*.log

failed=0
step() { printf '\n== %s\n' "$*"; }
fail() { printf 'FAIL %s\n' "$*"; failed=$((failed + 1)); }
pass() { printf 'ok   %s\n' "$*"; }
sums() { (cd "$1" && for f in *; do [ -f "$f" ] && cksum "$f"; done) }

step "build: debug, dev, ship"
for config in debug dev ship; do
    if "$kormake" $config >"$out/build-$config.log" 2>&1; then pass "$platform-$config"
    else fail "$platform-$config does not build (see $out/build-$config.log)"; tail -20 "$out/build-$config.log"; fi
done
[ $failed -ne 0 ] && { echo; echo "render-gate: the build failed; nothing else was run"; exit 1; }

step "tests (dev)"
if "$kormake" test dev >"$out/test.log" 2>&1; then pass "kormake test dev"
else fail "kormake test dev (see $out/test.log)"; tail -20 "$out/test.log"; fi

step "shaders: unchanged"
# The sources against main, which holds on any machine; the compiled files against the
# baseline taken on this one.
if git diff --quiet main -- modules/render/shaders examples/*/shaders 2>/dev/null; then pass "no shader source differs from main"
else fail "shader sources differ from main"; git diff --stat main -- modules/render/shaders examples/*/shaders; fi
sums "$dev/shaders" >"$out/shaders.sum"
if [ $baseline -eq 1 ]; then cp "$out/shaders.sum" "$keep/shaders.sum"; pass "baseline kept: $(wc -l <"$keep/shaders.sum") compiled shaders"
elif [ ! -f "$keep/shaders.sum" ]; then fail "no baseline ($keep/shaders.sum): run with --baseline at main first"
elif cmp -s "$out/shaders.sum" "$keep/shaders.sum"; then pass "compiled shaders byte-identical to the baseline"
else fail "compiled shaders differ from the baseline"; diff "$keep/shaders.sum" "$out/shaders.sum"; fi

step "gallery: exact against references/$backend"
(cd "$dev" && ./gallery$exe --out "$OLDPWD/$out" >"$OLDPWD/$out/gallery.log" 2>&1) || fail "gallery did not run (see $out/gallery.log)"
if [ ! -d "$refs" ]; then
    if [ $baseline -eq 1 ]; then
        echo "no committed references for $backend. Make them from this run and commit them:"
        echo "    $dev/picture$exe png $out $refs"
    else fail "no references in $refs"; fi
elif "$dev/picture$exe" compare "$out" "$refs" --threshold 0 --max-fraction 0 --max-channel 0 >"$out/compare.log" 2>&1; then
    pass "$(tail -1 "$out/compare.log")"
else fail "gallery differs"; cat "$out/compare.log"; fi

step "sort check (testbed, debug, validation on)"
(cd "$debug" && ./testbed$exe --check-sort --swarm --run-seconds 6 >"$OLDPWD/$out/testbed.log" 2>&1) || fail "testbed exited with an error"
grep -E "sort checks:|renderer down" "$out/testbed.log"
grep -q "sort checks: [1-9][0-9]* passed, 0 failed" "$out/testbed.log" && pass "sort checks" || fail "sort checks (see $out/testbed.log)"

step "arcfire demo (debug, validation on), with a screenshot"
(cd "$debug" && ./arcfire$exe --demo --check-sort --run-seconds 12 --screenshot "$OLDPWD/$out/arcfire.ppm" --screenshot-frame 300 \
    >"$OLDPWD/$out/arcfire.log" 2>&1) || fail "arcfire exited with an error"
grep -E "sort checks:|renderer down" "$out/arcfire.log"
grep -q "sort checks: [1-9][0-9]* passed, 0 failed" "$out/arcfire.log" && pass "sort checks" || fail "sort checks (see $out/arcfire.log)"
# Vulkan and D3D12 count their validation messages; Metal's validation aborts or logs.
if grep -q "validation errors" "$out/arcfire.log"; then
    grep -q " 0 validation errors, 0 warnings" "$out/arcfire.log" && pass "no validation message" || fail "validation messages (see $out/arcfire.log)"
fi
grep -E "^\[(ERROR|FATAL)" "$out/arcfire.log" "$out/testbed.log" && fail "errors logged"
[ -s "$out/arcfire.ppm" ] && pass "screenshot written" || fail "no screenshot"

step "bench (dev): arcfire --bench ring --dial rank=56, the last full report"
(cd "$dev" && ./arcfire$exe --bench ring --dial rank=56 --run-seconds 10 --no-vsync --prof-report >"$OLDPWD/$out/bench.log" 2>&1) || fail "the bench exited with an error"
# The last report before the run ended: the renderer's zones and the GPU's.
awk '/prof: over [0-9]+ frames/ { n++; block[n] = "" } n { block[n] = block[n] $0 "\n" } END { if (n > 1) n = n - 1; printf "%s", block[n] }' "$out/bench.log" |
    grep -E "prof: over|prof: (render|record|apply|gpu frame|gpu sort|gpu draw|gpu upload) " | sed 's/^\[ INFO\] prof: //' >"$out/bench.txt"
cat "$out/bench.txt"
if [ $baseline -eq 1 ]; then cp "$out/bench.txt" "$keep/bench.txt"; pass "baseline kept"
elif [ -f "$keep/bench.txt" ]; then echo "-- the baseline's:"; cat "$keep/bench.txt"; fi

echo
if [ $failed -eq 0 ]; then echo "render-gate: $platform $backend: all checks passed"; exit 0; fi
echo "render-gate: $platform $backend: $failed check(s) FAILED"
exit 1
