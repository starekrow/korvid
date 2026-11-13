#!/bin/sh
# kormake's own test: generates the fixture (tests/fixture, an engine root of its own) for a
# few targets and compares the Ninja files and the commands' output with tests/expected/.
# Paths are written as {root} (the fixture's copy) and {kormake}, the host's platform as
# {host}, so the files are the same on every POSIX host. Ninja, when it is on PATH, parses
# each file (a dry run).
#
#   tests/run.sh            bootstrap kormake, then compare
#   tests/run.sh --update   the same, writing tests/expected/ instead
#   KORMAKE=path tests/run.sh   use that binary, without bootstrapping
set -e
here=$(cd "$(dirname "$0")" && pwd)
update=
[ "$1" = "--update" ] && update=1
# The host's platform, as bootstrap.sh names it.
host=$(uname -s | tr '[:upper:]' '[:lower:]')
case "$host" in darwin) host=macos;; esac
if [ -z "$KORMAKE" ]; then
  "$here/../bootstrap.sh" >/dev/null
  KORMAKE="$here/../bin/$host/kormake"
fi
KORMAKE=$(cd "$(dirname "$KORMAKE")" && pwd)/$(basename "$KORMAKE")

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cp -R "$here/fixture" "$tmp/fixture"
root=$(cd "$tmp/fixture" && pwd -P)
out="$tmp/out"
mkdir -p "$out"

normalize() {
  sed -E -e "s|$root|{root}|g" -e "s|$KORMAKE|{kormake}|g" -e "s/(^|[^a-z])$host-(dev|ship)/\1{host}-\2/g"
}
# One command, its output and exit status, into commands.txt. With VAR=value words
# first, the command runs with those set.
run() {
  vars=
  while [ "${1#*=}" != "$1" ]; do vars="$vars $1"; shift; done
  echo "\$${vars} kormake $*" >>"$out/commands.txt.raw"
  rc=0
  (cd "$root" && env $vars "$KORMAKE" "$@") >>"$out/commands.txt.raw" 2>&1 || rc=$?
  echo "[exit $rc]" >>"$out/commands.txt.raw"
  echo >>"$out/commands.txt.raw"
}

for t in host-dev host-ship cross-dev; do
  run gen "$t"
  dir=$t
  [ "${t%%-*}" = host ] && dir=$host-${t#host-}
  normalize <"$root/build/$dir/build.ninja" >"$out/$t.ninja"
  if command -v ninja >/dev/null 2>&1; then
    ninja -n -C "$root/build/$dir" >/dev/null 2>"$tmp/ninja.err" || { echo "kormake tests: ninja cannot read $t's file:" >&2; cat "$tmp/ninja.err" >&2; exit 1; }
  fi
done
run check cross-dev
run graph cross-dev
run graph --why base cross-dev
run build extra host-dev
# A root's package that is not there: a problem check reports, graph marks and gen stops on.
run KORMAKE_PKG_DIR=nowhere check host-dev
run KORMAKE_PKG_DIR=nowhere graph host-dev
run KORMAKE_PKG_DIR=nowhere gen host-dev
normalize <"$out/commands.txt.raw" >"$out/commands.txt"
rm "$out/commands.txt.raw"

if [ -n "$update" ]; then
  rm -rf "$here/expected"
  cp -R "$out" "$here/expected"
  echo "kormake tests: wrote $here/expected"
  exit 0
fi
if diff -ru "$here/expected" "$out"; then
  echo "kormake tests: passed"
else
  echo "kormake tests: the output differs from tests/expected (above); if the change is meant, run tests/run.sh --update" >&2
  exit 1
fi
