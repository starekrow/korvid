#!/bin/sh
# patherizer's reference pictures (docs/PRDs/Krome-Slice-5-PRD.md, KP6a): resvg's drawings of
# the test corpus (written as SVG by patherizer_tests --write-svg) and of std's icons, at
# 16, 24, 48 and 200 pixels, into modules/patherizer/tests/references/. Run it when the corpus
# or the icons change, and commit what it writes. Needs resvg on PATH.
#
# resvg (tiny-skia) anti-aliases with 4 x 4 samples, so its edges are good to about an eighth
# of a pixel's coverage, and its stroker's offset curves are good to about a quarter of a
# pixel. Each reference is drawn at eight times its size and box-filtered down
# (kvscript, on premultiplied pixels): 32 x 32 samples a pixel, and the curves good to a
# thirtieth of a pixel.
#
#   tools/patherizer-refs.sh [build/linux-debug]
set -e
cd "$(dirname "$0")/.."
build=${1:-build/linux-debug}
tests="$build/bin/patherizer_tests"
[ -x "$tests" ] || { echo "patherizer-refs: no $tests (kormake test patherizer first)"; exit 2; }
command -v resvg >/dev/null || { echo "patherizer-refs: resvg is not on PATH"; exit 2; }
refs=modules/patherizer/tests/references
icons=modules/krome/assets/icons
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
"$tests" --write-svg "$tmp" >/dev/null
mkdir -p "$refs"
rm -f "$refs"/*.png
# Each picture at 8x, listed with its final size, then all of them reduced in one run.
list="$tmp/list.txt"
: > "$list"
draw() { # svg, name
    for size in 16 24 48 200; do
        big=$((size * 8))
        resvg -w $big -h $big "$1" "$tmp/$2_$size.big.png"
        echo "$tmp/$2_$size.big.png $refs/$2_$size.png $size" >> "$list"
    done
}
for svg in "$tmp"/*.svg; do draw "$svg" "$(basename "$svg" .svg)"; done
for name in $(grep -v '^#' modules/krome/icons/NAMES); do draw "$icons/$name.svg" "icon_$name"; done
cat > "$tmp/reduce.lua" <<'LUA'
local n = 0
for line in io.lines(arg[1]) do
    local from, to, size = line:match("(%S+) (%S+) (%d+)")
    kv.image.load(from):resize(tonumber(size), tonumber(size), "box"):save(to)
    n = n + 1
end
print(n)
LUA
n=$(tools/kormake/bin/linux/kormake run "$tmp/reduce.lua" "$list" | tail -1)
echo "patherizer-refs: $n pictures from resvg $(resvg --version), at 8x and reduced, in $refs"
