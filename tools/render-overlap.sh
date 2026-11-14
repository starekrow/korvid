#!/usr/bin/env bash
# How much the three renderer backends still hold in common (docs/PRDs/Engine-Slice-9-PRD.md):
# distinct code lines (comments, lone braces and bare control words left out, whitespace and
# the old rv_/rd_/mt_ prefixes normalised) that appear in all three backends' sources, and
# each backend's size. The slice began at 686 identical in all three (Nov 13, main).
#   tools/render-overlap.sh [git-ref]      the working tree, or a commit
set -u
export LC_ALL=C
cd "$(dirname "$0")/.."
ref=${1:-}
files() { if [ -n "$ref" ]; then git ls-tree -r --name-only "$ref" -- "modules/renderer-$1/src"; else find "modules/renderer-$1/src" -type f; fi | grep -E '\.(cc|mm|h)$' | sort; }
show()  { if [ -n "$ref" ]; then git show "$ref:$1"; else cat "$1"; fi; }
norm()  { for f in $(files "$1"); do show "$f"; done |
          sed -E 's/\b(rv|rd|mt|rb)_/rx_/g; s/[[:space:]]+/ /g; s/^ //; s/ $//' |
          grep -vE '^(//.*)?$' | grep -vE '^[{}]|^(return|break|continue|else)' | sort -u; }
raw()   { for f in $(files "$1"); do show "$f"; done | wc -l; }
t=$(mktemp -d)
for b in vulkan metal d3d12; do norm $b >"$t/$b"; done
printf 'lines in src/:        vulkan %5d   metal %5d   d3d12 %5d   total %d\n' "$(raw vulkan)" "$(raw metal)" "$(raw d3d12)" $(( $(raw vulkan) + $(raw metal) + $(raw d3d12) ))
printf 'distinct code lines:  vulkan %5d   metal %5d   d3d12 %5d\n' "$(wc -l <"$t/vulkan")" "$(wc -l <"$t/metal")" "$(wc -l <"$t/d3d12")"
printf 'identical in all three: %d\n' "$(comm -12 "$t/vulkan" "$t/metal" | comm -12 - "$t/d3d12" | wc -l)"
rm -r "$t"
