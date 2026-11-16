#!/bin/sh
# The CPU renderer's numbers on this machine (docs/PRDs/CPU-Renderer-PRD.md, "Performance"):
# each workload under --renderer cpu at 1280x720 and 1920x1080, on every core and on one,
# with the widest kernels; then at 1920x1080 on every core with the baseline kernels. A
# Markdown table on stdout, one row a run, from the backend's KV_CPU_STATS line (the means
# of its last 60 frames) and frameloop's frame rate. Windows open while it runs.
#
#   tools/cpu-numbers.sh [build/linux-dev] [WORKLOAD-PREFIX] > numbers.md
set -e
build=${1:-build/linux-dev}
bin="$build/bin"
[ -x "$bin/arcfire" ] || { echo "cpu-numbers: no $bin/arcfire (build the dev config first)" >&2; exit 2; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

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
  # shellcheck disable=SC2086
  (cd "$bin" && KV_CPU_STATS=60 KV_CPU_ISA="$5" timeout 60 ./$2 --renderer cpu --size "$3" $threads </dev/null >"$log" 2>&1) || true
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
grep -q avx2 /proc/cpuinfo 2>/dev/null || widest=base
only=${2:-}
echo "$workloads" | while IFS='|' read -r name cmd; do
  case $name in "$only"*) ;; *) continue ;; esac
  for size in 1280x720 1920x1080; do
    for threads in "" 1; do run "$name" "$cmd" "$size" "$threads" "$widest"; done
  done
  [ "$widest" = base ] || run "$name" "$cmd" 1920x1080 "" base
done
