#!/bin/sh
# Material Symbols for krome/std (docs/PRDs/Krome-Slice-5-PRD.md, "Icons in krome/std"): the
# SVG files go to ../assets/icons/, and this directory keeps what made them (NAMES, COMMIT)
# and their licence. The
# Outlined style, weight 400, unfilled, at the 20 px optical size, from
# google/material-design-icons at the commit in COMMIT (or, with --latest, the current one,
# written there). Each file gets fill="currentColor" on its root element: Material's files
# have no fill, which SVG paints black, and std's icons take the node's colour.
#
#   sh modules/krome/icons/vendor.sh [--latest] [name...]
#
# With no names, every icon in NAMES. Needs curl. LICENSE is the repository's (Apache 2.0).
set -e
cd "$(dirname "$0")"
repo=google/material-design-icons
if [ "${1:-}" = "--latest" ]; then
    shift
    curl -fsSL "https://api.github.com/repos/$repo/commits/master" | sed -n 's/^  "sha": "\([0-9a-f]*\)".*/\1/p' | head -1 > COMMIT
fi
commit=$(cat COMMIT)
[ -n "$commit" ] || { echo "vendor.sh: no commit (run with --latest once)"; exit 2; }
names="$*"
[ -n "$names" ] || names=$(grep -v '^#' NAMES)
for n in $names; do
    curl -fsSL "https://raw.githubusercontent.com/$repo/$commit/symbols/web/$n/materialsymbolsoutlined/${n}_20px.svg" -o "$n.svg.tmp"
    sed 's/<svg /<svg fill="currentColor" /' "$n.svg.tmp" > "../assets/icons/$n.svg"
    rm -f "$n.svg.tmp"
done
[ -f LICENSE ] || curl -fsSL "https://raw.githubusercontent.com/$repo/$commit/LICENSE" -o LICENSE
echo "vendor.sh: $(echo $names | wc -w) icons at $commit"
