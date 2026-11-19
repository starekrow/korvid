#!/bin/sh
# goldberg's window, driven over this fixture: each page pressed and checked against the session
# (`--demo`), pictures in build/<target>/bench/window/. The fixture is copied first: the demo
# saves its rule file. Exits as the demo does.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
target=${1:-linux-dev}
work="$root/build/$target/bench/window"
rm -rf "$work"
mkdir -p "$work"
cp -R "$here/assets" "$work/assets"
exec "$root/build/$target/bin/goldberg" --demo --shots "$work/shots" watch fixture --root "fix=$work/assets" --out "$work/out" --project "$work" --window
