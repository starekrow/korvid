#!/bin/sh
# kormake's own test: generates the fixture (tests/fixture, an engine root of its own) for a
# few targets and compares the Ninja files and the commands' output with tests/expected/.
# Paths are written as {root} (the fixture's copy) and {kormake}, the host's platform as
# {host}, so the files are the same on every host, Windows (under Git Bash) too. Ninja, when it is on PATH, parses
# each file (a dry run).
#
#   tests/run.sh            bootstrap kormake, then compare
#   tests/run.sh --update   the same, writing tests/expected/ instead
#   KORMAKE=path tests/run.sh   use that binary, without bootstrapping
set -e
here=$(cd "$(dirname "$0")" && pwd)
update=
[ "$1" = "--update" ] && update=1
# The host's platform, as the bootstrap scripts name it. On Windows (Git Bash, MSYS2, Cygwin)
# kormake is bootstrap.bat's, built with MSVC, and spells paths C:/like/this: native
# turns a POSIX path into that spelling, so the outputs normalize the same as elsewhere.
host=$(uname -s | tr '[:upper:]' '[:lower:]')
exe=
native() { printf '%s' "$1"; }
case "$host" in
  darwin) host=macos;;
  mingw*|msys*|cygwin*) host=windows; exe=.exe; native() { cygpath -m "$1"; };;
esac
if [ -z "$KORMAKE" ]; then
  if [ "$host" = windows ]; then "$here/../bootstrap.bat" >/dev/null; else "$here/../bootstrap.sh" >/dev/null; fi
  KORMAKE="$here/../bin/$host/kormake$exe"
fi
KORMAKE=$(native "$(cd "$(dirname "$KORMAKE")" && pwd)/$(basename "$KORMAKE")")

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cp -R "$here/fixture" "$tmp/fixture"
cp -R "$here/project" "$tmp/project"
root=$(native "$(cd "$tmp/fixture" && pwd -P)")
proj=$(native "$(cd "$tmp/project" && pwd -P)")
out="$tmp/out"
mkdir -p "$out"

# A string as an extended regular expression that matches only itself.
re() { printf '%s' "$1" | sed -e 's/[][\.*^$+?(){}]/\\&/g'; }
# A path as Ninja writes it on a build line: $, space and colon escaped (C$:/Users/A$ B).
ninja_path() { printf '%s' "$1" | sed -e 's/\$/$$/g' -e 's/ /$ /g' -e 's/:/$:/g'; }
# What differs by host spelling only. On Windows kormake quotes with "..." where POSIX
# quotes with '...' and names a program host\bin\x (the expected files hold neither " nor
# \), and the graph's columns pad around "windows" where they pad around "linux" or
# "macos", two letters shorter: the padding is given back so one expected set serves all.
host_spelling() {
  if [ "$host" = windows ]; then
    pad=$(printf '%*s' $((${#host} - 5)) '')
    # The fixture's cook is copy_assets.sh, or copy_assets.bat beside it on Windows.
    sed -E -e "s|\"|'|g" -e "s/(\{host\}-(dev|ship))(  +)/\1\3$pad/g" -e "s|copy_assets\.bat|copy_assets.sh|g"
  else
    cat
  fi
}
# Backslashes first, before the paths are looked for: kormake writes some paths native
# (C:\...), and the expected files hold no backslash.
slashes() { if [ "$host" = windows ]; then sed -e 's|\\|/|g'; else cat; fi; }
normalize() {
  slashes | sed -E -e "s|$(re "$(ninja_path "$root")")|{root}|g" -e "s|$(re "$root")|{root}|g" \
         -e "s|$(re "$(ninja_path "$proj")")|{project}|g" -e "s|$(re "$proj")|{project}|g" \
         -e "s|$(re "$(ninja_path "$KORMAKE")")|{kormake}|g" -e "s|$(re "$KORMAKE")|{kormake}|g" \
         -e "s/(^|[^a-z])$host-(dev|ship)/\1{host}-\2/g" \
         -e "s/_${host}([ _]|\$)/_{host}\1/g" | host_spelling # a rule variant's name: tool_x_y_<platform>[_<module>]
}
# One command, its output and exit status, into commands.txt. With VAR=value words
# first, the command runs with those set. It runs in $cwd and is logged to $log.
cwd=$root
log=commands.txt
run() {
  vars=
  while [ "${1#*=}" != "$1" ]; do vars="$vars $1"; shift; done
  echo "\$${vars} kormake $*" >>"$out/$log.raw"
  rc=0
  (cd "$cwd" && env $vars "$KORMAKE" "$@") >>"$out/$log.raw" 2>&1 || rc=$?
  echo "[exit $rc]" >>"$out/$log.raw"
  echo >>"$out/$log.raw"
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

# The project beside the fixture (tests/project): externals a module and a korvid.config
# define, one of them asked of pkg-config (a fake one, a shell script: on Windows this part
# is not run, and the expected files stand in for it).
if [ "$host" = windows ]; then
  cp "$here"/expected/project-* "$out/"
else
  cwd=$proj
  log=project-commands.txt
  fake=KORMAKE_PKG_CONFIG=pkgs/fake-pkg-config
  run $fake gen host-dev
  normalize <"$proj/build/$host-dev/build.ninja" >"$out/project-host-dev.ninja"
  if command -v ninja >/dev/null 2>&1; then
    ninja -n -C "$proj/build/$host-dev" >/dev/null 2>"$tmp/ninja.err" || { echo "kormake tests: ninja cannot read the project's file:" >&2; cat "$tmp/ninja.err" >&2; exit 1; }
  fi
  run $fake graph host-dev
  # The package pkg-config does not have: check says who needs it and how to get it.
  run $fake FAKE_PKG_MISSING=libfakepw-0.3 check host-dev
  # No pkg-config at all: the external's own fields stand.
  run KORMAKE_PKG_CONFIG=pkgs/no-such-program gen host-dev
  grep -e '-lfakepw' "$proj/build/$host-dev/build.ninja" >>"$out/$log.raw"
  echo >>"$out/$log.raw"
  # Not the host's platform: pkg-config is not asked, and the project defines nothing for it.
  run $fake check cross-dev
  normalize <"$out/$log.raw" >"$out/$log"
  rm "$out/$log.raw"
fi

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
