#!/bin/sh
# The job pool's, the allocator's and the scheduler's numbers on this machine: cpukiller's
# table and its thread sweep (examples/cpukiller/README.md says what each row is), as
# Markdown on stdout under a heading that names the machine, the build and the power
# setting they were taken with. A number without those is not a number: a laptop on its
# balanced plan moves its clocks, and the same case can differ by a fifth between runs.
# Plug it in, pick the performance plan, close the browser, and say so if you did not.
#
# Takes a few minutes. Nothing opens a window.
#
#   tools/concurrency-numbers.sh [build/<platform>-<config>] [cpukiller arguments] > numbers.md
#
# The build is <platform>-dev unless named; ship has no checks and is the one to quote for
# speed, dev the one people run all day.
set -e
case $(uname -s) in
  Darwin) platform=macos ;;
  MINGW*|MSYS*|CYGWIN*) platform=windows ;;
  *) platform=linux ;;
esac
build=build/$platform-dev
case ${1:-} in build/*) build=$1; shift ;; esac
exe="$build/bin/cpukiller"
[ -x "$exe" ] || [ -x "$exe.exe" ] || { echo "concurrency-numbers: no $exe (kormake cpukiller ${build##*-})" >&2; exit 2; }

case $platform in
  macos)
    cpu=$(sysctl -n machdep.cpu.brand_string)
    cores="$(sysctl -n hw.physicalcpu) cores, $(sysctl -n hw.logicalcpu) threads"
    power=$(pmset -g batt 2>/dev/null | head -1 | sed -E "s/.*'(.*)'.*/\1/")
    os="macOS $(sw_vers -productVersion)" ;;
  windows)
    cpu=$(powershell -NoProfile -Command "(Get-CimInstance Win32_Processor).Name.Trim()" | tr -d '\r')
    cores=$(powershell -NoProfile -Command "\$p = Get-CimInstance Win32_Processor; '{0} cores, {1} threads' -f \$p.NumberOfCores, \$p.NumberOfLogicalProcessors" | tr -d '\r')
    power=$(powercfg //getactivescheme | sed -E 's/.*\((.*)\).*/\1/' | tr -d '\r')
    power="$power plan, $(powershell -NoProfile -Command "if ((Get-CimInstance Win32_Battery -ErrorAction SilentlyContinue).BatteryStatus -eq 1) { 'on battery' } else { 'on mains' }" | tr -d '\r')"
    os=$(powershell -NoProfile -Command "(Get-CimInstance Win32_OperatingSystem).Caption.Trim()" | tr -d '\r') ;;
  *)
    cpu=$(sed -n 's/^model name[^:]*: //p' /proc/cpuinfo | head -1)
    cores="$(lscpu -p=CORE 2>/dev/null | grep -v '^#' | sort -u | wc -l | tr -d ' ') cores, $(nproc) threads"
    power="governor $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)"
    os="$(uname -sr)" ;;
esac

echo "## Concurrency numbers"
echo
echo "- Machine: $cpu ($cores)"
echo "- OS: $os"
echo "- Power: $power"
echo "- Build: ${build#build/}, at $(git rev-parse --short HEAD 2>/dev/null || echo unknown)$(git diff --quiet 2>/dev/null || echo ', with uncommitted changes')"
echo "- Date: $(date +%Y-%m-%d)"
echo
echo "Medians of repeated, alternated runs; \"spread\" is the pool's slowest repeat less its"
echo "fastest, as a share of its median. \"serial\" is a plain loop. \"reference\" is the least"
echo "that could be done: for the pool, threads that spin and claim chunks with one atomic add;"
echo "for the allocator, the C runtime's malloc on the same threads; for the engine, the same"
echo "systems over bare arrays on one thread."
echo
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
"$exe" --md "$@" >"$tmp" 2>&1
echo '```'
sed -n '/^engine breakdown/,/^$/p' "$tmp"
echo '```'
echo
sed -n '/^| scenario/,/^$/p' "$tmp"
echo "### Speedup by thread count"
echo
"$exe" --md --sweep "$@" 2>/dev/null | sed -n '/^| speedup/,$p' | sed '/^memory at exit/d'
