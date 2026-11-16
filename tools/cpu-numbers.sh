#!/bin/sh
# The CPU renderer's numbers on this machine (docs/PRDs/CPU-Renderer-PRD.md, "Performance"):
# each workload under --renderer cpu at 1280x720 and 1920x1080, on every core and on one,
# with the widest kernels; then at 1920x1080 on every core with the baseline kernels. A
# Markdown table on stdout, one row a run, from the backend's KV_CPU_STATS line (the means
# of its last 60 frames) and frameloop's frame rate. Windows open while it runs, silent
# (SDL's dummy audio driver unless SDL_AUDIO_DRIVER says otherwise).
#
# Sizes are pixels: on a display that scales (a Mac's Retina screen, where --size is in
# points), each size is divided by the window's scale, so the surface is the size asked for.
#
#   tools/cpu-numbers.sh [build/<platform>-dev] [WORKLOAD-PREFIX] > numbers.md
set -e
case $(uname -s) in
  Darwin) platform=macos ;;
  *) platform=linux ;;
esac
build=${1:-build/$platform-dev}
bin="$build/bin"
[ -x "$bin/arcfire" ] || { echo "cpu-numbers: no $bin/arcfire (build the dev config first)" >&2; exit 2; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
export SDL_AUDIO_DRIVER="${SDL_AUDIO_DRIVER:-dummy}"

# A command, killed after $1 seconds: timeout where there is one (GNU's, or Homebrew's
# gtimeout), else perl's alarm (macOS has no timeout).
limit() {
  secs=$1; shift
  if command -v timeout >/dev/null 2>&1; then timeout "$secs" "$@"
  elif command -v gtimeout >/dev/null 2>&1; then gtimeout "$secs" "$@"
  else perl -e 'alarm shift; exec @ARGV or die "$ARGV[0]: $!\n"' "$secs" "$@"
  fi
}

# The window's pixels per --size unit, from the size it reports opening at.
px=$(cd "$bin" && limit 30 ./arcfire --demo --renderer cpu --size 1280x720 --run-seconds 1 </dev/null 2>&1 |
  sed -nE 's/.*window: opened .* ([0-9]+)x[0-9]+ px.*/\1/p' | head -1)
scale=$(awk -v px="${px:-1280}" 'BEGIN { print px / 1280 }')
[ "$scale" = 1 ] || echo "cpu-numbers: the window scales by $scale; sizes are asked for in its units" >&2

# name | program and its arguments (each runs about ten seconds, or quits by itself)
workloads='kromedemo hover|kromedemo --hover
kromedemo bench (2,000 nodes a tick)|kromedemo --bench --run-seconds 10
kromescreens bench (10,000 steps a tick)|kromescreens --bench
arcfire demo, shimmer|arcfire --demo --dial shimmer=1 --run-seconds 12
arcfire demo, --cpu-shaders none|arcfire --demo --dial shimmer=1 --cpu-shaders none --run-seconds 12
arcfire --bench ring rank=56|arcfire --bench ring --dial rank=56 --run-seconds 10'

echo "| Workload | Size | Threads | Kernels | fps | Record ms | UI lists ms | Pass end ms | Present ms | Tiles drawn / skipped |"
echo "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |"
run() { # name, command, size, threads, isa
  log="$tmp/run.log"
  threads=""
  [ "$4" = "1" ] && threads="--threads 1"
  units=$(echo "$3" | awk -F x -v s="$scale" '{ printf "%dx%d", $1 / s + 0.5, $2 / s + 0.5 }')
  # shellcheck disable=SC2086
  (cd "$bin" && KV_CPU_STATS=60 KV_CPU_ISA="$5" limit 60 ./$2 --renderer cpu --size "$units" $threads </dev/null >"$log" 2>&1) || true
  got=$(sed -nE 's/.*window: opened .* ([0-9]+x[0-9]+) px.*/\1/p' "$log" | head -1)
  [ -z "$got" ] || [ "$got" = "$3" ] || echo "cpu-numbers: $1 at $3 opened at $got px" >&2
  fps=$(grep -E 'frameloop: [0-9]+ fps' "$log" | tail -1 | sed -E 's/.*frameloop: ([0-9]+) fps.*/\1/')
  line=$(grep 'cpu: stats:' "$log" | tail -1)
  if [ -z "$line" ]; then
    echo "| $1 | $3 | ${4:-all} | $5 | ${fps:-?} | (no stats line) | | | | |"
    return
  fi
  rec=$(echo "$line" | sed -E 's/.*record ([0-9.]+) ms.*/\1/')
  ui=$(echo "$line" | sed -E 's/.*UI lists ([0-9.]+),.*/\1/')
  pe=$(echo "$line" | sed -E 's/.*pass end ([0-9.]+),.*/\1/')
  pr=$(echo "$line" | sed -E 's/.*present ([0-9.]+) ms.*/\1/')
  tiles=$(echo "$line" | sed -E 's/.*tiles ([0-9.]+) drawn, ([0-9.]+) skipped.*/\1 \/ \2/')
  echo "| $1 | $3 | ${4:-all} | $5 | ${fps:-?} | $rec | $ui | $pe | $pr | $tiles |"
}
widest=avx2
if [ -r /proc/cpuinfo ]; then grep -q avx2 /proc/cpuinfo || widest=base
else sysctl -n machdep.cpu.leaf7_features 2>/dev/null | grep -qi avx2 || widest=base
fi
only=${2:-}
echo "$workloads" | while IFS='|' read -r name cmd; do
  case $name in "$only"*) ;; *) continue ;; esac
  for size in 1280x720 1920x1080; do
    for threads in "" 1; do run "$name" "$cmd" "$size" "$threads" "$widest"; done
  done
  [ "$widest" = base ] || run "$name" "$cmd" 1920x1080 "" base
done
