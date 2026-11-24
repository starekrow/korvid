#!/usr/bin/env bash
# Same input, same game (docs/principles.md, 1), checked: rocks --demo hashed every tick
# in the configurations that must agree (debug and ship builds, 1 and 16 threads, early
# reads ordered and from snapshots, the CPU renderer), compared tick by tick, and
# arcfire's recordings replayed. Builds what it runs. Needs a display (rocks and arcfire
# open windows); about three minutes.
#
#   tools/determinism-check.sh            everything
#   tools/determinism-check.sh --quick    rocks at debug only: threads and early reads
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root" || exit 2
quick=0
case "$(uname -s)" in Darwin) host=macos ;; *) host=linux ;; esac
[ "${1:-}" = "--quick" ] && quick=1
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

configs=(debug)
[ $quick -eq 0 ] && configs+=(dev ship)
for c in "${configs[@]}"; do
    (cd examples && kormake rocks "$c" >/dev/null 2>&1) || { echo "determinism-check: rocks $c does not build" >&2; exit 2; }
done
[ $quick -eq 0 ] && { (cd examples && kormake arcfire dev >/dev/null 2>&1) || { echo "determinism-check: arcfire dev does not build" >&2; exit 2; }; }

# rocks --demo to sim tick 1,500, each tick hashed: the same run however fast the machine
# draws (--run-ticks counts sim ticks; --no-vsync keeps a hidden window from slowing it).
run() { # name, exe, args...
    local name=$1 exe=$2; shift 2
    timeout 120 "$exe" --demo --rate 4 --no-vsync --run-ticks 1500 --hash "$@" 2>&1 |
        awk '/^\[ INFO\] hash: tick / {print $5, $6}' > "$tmp/$name"
}
run t1        build/$host-debug/bin/rocks --threads 1
run t16       build/$host-debug/bin/rocks --threads 16
run order     build/$host-debug/bin/rocks --threads 16 --early-reads order
run snapshot  build/$host-debug/bin/rocks --threads 16 --early-reads snapshot
if [ $quick -eq 0 ]; then
    run dev_t16   build/$host-dev/bin/rocks --threads 16
    run ship_t1   build/$host-ship/bin/rocks --threads 1
    run ship_t16  build/$host-ship/bin/rocks --threads 16
    run ship_cpu  build/$host-ship/bin/rocks --threads 16 --renderer cpu
fi

failed=0
for f in "$tmp"/*; do
    name=$(basename "$f")
    [ "$name" = t1 ] && continue
    # Ticks both runs reached, compared; a run's own length depends on the machine.
    result=$(awk 'NR == FNR {h[$1] = $2; next} ($1 in h) {n++; if (h[$1] != $2 && !d) d = $1} END {printf "%d %s", n, d ? d : "-"}' "$tmp/t1" "$f")
    common=${result% *}; first=${result#* }
    if [ "$common" -lt 1400 ]; then
        echo "rocks $name: only $common ticks in common with t1: did it run?"; failed=1
    elif [ "$first" != "-" ]; then
        echo "rocks $name: differs from t1 at tick $first"; failed=1
    else
        echo "rocks $name: the same as t1, $common ticks"
    fi
done

if [ $quick -eq 0 ]; then
    for r in examples/arcfire/tests/replays/*.kvr; do
        line=$(timeout 300 build/$host-dev/bin/arcfire --replay "$r" --rate 4 --threads 16 2>&1 | grep 'replay: ' | tail -1)
        echo "arcfire $(basename "$r"): ${line#*replay: }"
        case "$line" in *matched*) ;; *) failed=1 ;; esac
    done
fi

if [ $failed -ne 0 ]; then echo "determinism-check: failed" >&2; exit 1; fi
echo "determinism-check: ok"
