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
# The tool tables every gen of the host target writes (kormake link tools), kept here.
KORVID_LINK_CACHE=$(native "$tmp/cache")
export KORVID_LINK_CACHE

# A string as an extended regular expression that matches only itself.
re() { printf '%s' "$1" | sed -e 's/[][\.*^$+?(){}]/\\&/g'; }
# A path as Ninja writes it on a build line: $, space and colon escaped (C$:/Users/A$ B).
ninja_path() { printf '%s' "$1" | sed -e 's/\$/$$/g' -e 's/ /$ /g' -e 's/:/$:/g'; }
# What differs by host spelling only. On Windows kormake quotes with "..." where POSIX
# quotes with '...' and names a program host\bin\x (the expected files hold neither " nor
# \), and the graph's columns pad around "windows" where they pad around "linux" or
# "macos", two letters shorter: the padding is given back so one expected set serves all.
host_spelling() {
  # The project's player says how to fetch fakepw per platform (its module.lua): this
  # host's words, and only these, are {fetch}, so another platform's would still differ.
  case "$host" in
    linux) fetch='sudo apt install libfakepw-0.3-dev';;
    macos) fetch='brew install fakepw';;
    windows) fetch='vcpkg install fakepw';;
    *) fetch='{fetch}';;
  esac
  if [ "$host" = windows ]; then
    pad=$(printf '%*s' $((${#host} - 5)) '')
    # The fixture's cook is copy_assets.sh, or copy_assets.bat beside it on Windows; the
    # project's fake pkg-config likewise.
    sed -E -e "s|\"|'|g" -e "s/(\{host\}-(dev|ship))(  +)/\1\3$pad/g" -e "s|copy_assets\.bat|copy_assets.sh|g" \
           -e "s|fake-pkg-config\.bat|fake-pkg-config|g" -e "s|^$(re "$fetch")\$|{fetch}|"
  else
    sed -e "s|^$(re "$fetch")\$|{fetch}|"
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
         -e "s/_${host}([ _]|\$)/_{host}\1/g" \
         -e "s|$(re "$KORVID_LINK_CACHE")|{cache}|g" -e "s|/bin/$host/kormake|/bin/{host}/kormake|g" \
         -e "s|(^watch	[^	]*)	[-0-9]+	[-0-9]+\$|\1	{size}	{mtime}|" -e "s|/roots/[0-9a-f]{16}\.txt|/roots/{hash}.txt|" \
         -e "s|(\.exe)?(	\|\$)|\2|g" | host_spelling # a rule variant's name: tool_x_y_<platform>[_<module>]
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
# A sanitized build: a directory of its own, the flags on the target's instances only.
run gen host-dev --sanitize undefined,address
normalize <"$root/build/$host-dev+address+undefined/build.ninja" >"$out/host-dev-sanitize.ninja"
run gen host-dev --sanitize address,thread
run gen host-dev --sanitize memory
run gen cross-dev --sanitize address
run check host-dev --sanitize address
run check cross-dev
run graph cross-dev
run graph --why base cross-dev
run build extra host-dev
# A root's package that is not there: a problem check reports, graph marks and gen stops on.
run KORMAKE_PKG_DIR=nowhere check host-dev
run KORMAKE_PKG_DIR=nowhere graph host-dev
run KORMAKE_PKG_DIR=nowhere gen host-dev
# The tool table the host target's gen wrote: each published command and its binary.
run _link write .
normalize <"$KORVID_LINK_CACHE/roots/$(ls "$KORVID_LINK_CACHE/roots" | head -n 1)" >"$out/link-table.txt"
rm -rf "$KORVID_LINK_CACHE"
run _link engine .
# What a module may not publish: a library, a name that is not a command's, kormake's own,
# and one command published twice on one side.
packer="$tmp/fixture/modules/packer/module.lua"
cp "$packer" "$tmp/packer.keep"
sed -e 's|output    = "exe",|output    = "lib",|' "$tmp/packer.keep" >"$packer"
run gen host-dev
sed -e 's|publish   = true,|publish   = "no/slash",|' "$tmp/packer.keep" >"$packer"
run gen host-dev
sed -e 's|publish   = true,|publish   = "kormake",|' "$tmp/packer.keep" >"$packer"
run gen host-dev
sed -e 's|publish   = true,|publish   = "stamper",|' "$tmp/packer.keep" >"$packer"
run gen host-dev
cp "$tmp/packer.keep" "$packer"
# A private module with a public module's name: in the engine's own view, a clash.
cp -R "$tmp/fixture/samples/demo" "$tmp/fixture/samples/base"
sed -e 's|module "demo"|module "base"|' "$tmp/fixture/samples/demo/module.lua" >"$tmp/fixture/samples/base/module.lua"
run gen host-dev
rm -rf "$tmp/fixture/samples/base"
normalize <"$out/commands.txt.raw" >"$out/commands.txt"
rm "$out/commands.txt.raw"

# The project beside the fixture (tests/project): externals a module and a korvid.config
# define, one of them asked of pkg-config (a fake one, a shell script; on Windows the .bat
# beside it, which cmd can run, hands it to Git Bash's sh).
{
  cwd=$proj
  log=project-commands.txt
  fake=KORMAKE_PKG_CONFIG=pkgs/fake-pkg-config
  [ "$host" = windows ] && fake=$fake.bat
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
  # codegen_sources naming a file the module does not have.
  codec="$tmp/project/modules/codec/module.lua"
  cp "$codec" "$tmp/codec.keep"
  sed -e 's|src/decode.cc|src/nowhere.cc|' "$tmp/codec.keep" >"$codec"
  run $fake gen host-dev
  cp "$tmp/codec.keep" "$codec"
  # A private module of the engine's is not there for a project to use.
  player="$tmp/project/modules/player/module.lua"
  cp "$player" "$tmp/player.keep"
  sed -e 's|deps      = { "base", "codec" }|deps      = { "base", "codec", "demo" }|' "$tmp/player.keep" >"$player"
  run $fake gen host-dev
  cp "$tmp/player.keep" "$player"
  # The project's table: its packer wins the name, the engine's private stamp is there
  # to run, and the engine's packer is the candidate that lost.
  run $fake _link write .
  normalize <"$KORVID_LINK_CACHE/roots/$(ls "$KORVID_LINK_CACHE/roots" | grep -v '\.new$' | while read f; do grep -q "^anchor	$proj\$" "$KORVID_LINK_CACHE/roots/$f" && echo "$f"; done)" >"$out/project-link-table.txt"
  run _link engine .
  # The engine's private tool can be named, not depended on nor used as a tool.
  run $fake graph stamp host-dev
  sed -e 's|deps      = { "base", "codec" }|deps      = { "base", "codec", "stamp" }|' "$tmp/player.keep" >"$player"
  run $fake gen host-dev
  sed -e 's|deps      = { "base", "codec" },|deps      = { "base", "codec" }, tools = { "stamp" },|' "$tmp/player.keep" >"$player"
  run $fake gen host-dev
  cp "$tmp/player.keep" "$player"
  normalize <"$out/$log.raw" >"$out/$log"
  rm "$out/$log.raw"
}

# Linked tools (kormake link tools): the shim, in a home, cache and bin directory of the
# test's own. The fixture gets this kormake and shim where an engine keeps them; its tools
# are stubs that print what they were run as, and Ninja is a stub that says what it was
# asked, so nothing is compiled.
if [ "$host" != windows ]; then
  cwd=$root
  log=link-commands.txt
  lt="$tmp/lt"
  mkdir -p "$lt/home" "$lt/bin" "$lt/fake" "$lt/elsewhere" "$lt/onpath"
  export KORVID_LINK_HOME="$lt/home" KORVID_LINK_BIN="$lt/bin"
  mkdir -p "$root/tools/kormake/bin/$host" "$root/tools/kormake/src"
  cp "$KORMAKE" "$root/tools/kormake/bin/$host/kormake"
  cp "$here/../bin/$host/kvbinlink" "$root/tools/kormake/bin/$host/kvbinlink"
  cp "$here/../src/kvbinlink.c" "$root/tools/kormake/src/kvbinlink.c"
  stub() { mkdir -p "$(dirname "$1")"; printf '#!/bin/sh\necho "%s: ran as $0 ($KORVID_LINK_NAME) with $*"\nexit %s\n' "$2" "${3:-0}" >"$1"; chmod +x "$1"; }
  printf '#!/bin/sh\necho "fake ninja $*"\ncase "$*" in -n*) echo "ninja: no work to do.";; esac\nexit 0\n' >"$lt/fake/ninja"
  chmod +x "$lt/fake/ninja"
  stub "$lt/onpath/packer" "the other packer on PATH"
  # A PATH of the test's own: the links, the fake Ninja, and the system's programs.
  keep_path=$PATH
  export PATH="$lt/bin:$lt/fake:/usr/bin:/bin"
  # A shim run: as its link, from $cwd, standard output and error kept apart.
  shim() {
    vars=
    while [ "${1#*=}" != "$1" ]; do vars="$vars $1"; shift; done
    echo "\$${vars} $*" >>"$out/$log.raw"
    rc=0
    (cd "$cwd" && env $vars "$@") >"$tmp/shim.out" 2>"$tmp/shim.err" || rc=$?
    sed -e 's/^/  out: /' "$tmp/shim.out" >>"$out/$log.raw"
    sed -e 's/^/  err: /' "$tmp/shim.err" >>"$out/$log.raw"
    echo "[exit $rc]" >>"$out/$log.raw"
    echo >>"$out/$log.raw"
  }
  run link tools
  ls "$lt/bin" >>"$out/$log.raw"
  stub "$root/build/$host-dev/bin/packer" "the fixture's packer" 4
  stub "$root/build/$host-dev/bin/stamp" "the fixture's stamp"
  # Built first (the fake Ninja's words on standard error), then run: its status is the tool's.
  shim packer one "two words"
  shim stamper
  shim KORVID_LINK_BUILD=never packer
  shim KORVID_LINK_HELD="$root/build/$host-dev" packer
  shim packer --diagnose-tool-lookup
  cwd=$root/samples/demo shim stamp --diagnose-tool-lookup
  shim kormake --diagnose-tool-lookup
  shim "$lt/home/kvbinlink" nosuch
  shim "$lt/home/kvbinlink" packer x
  # Outside any tree: the next program of the name on PATH, or nothing.
  cwd=$lt/elsewhere shim packer --diagnose-tool-lookup
  cwd=$lt/elsewhere shim packer
  cwd=$lt/elsewhere shim PATH="$PATH:$lt/onpath" packer --diagnose-tool-lookup
  cwd=$lt/elsewhere shim PATH="$PATH:$lt/onpath" packer x
  # The project: refused until it is linked, then its own packer.
  cwd=$proj
  stub "$proj/build/$host-dev/bin/mypacker" "the project's packer"
  shim KORMAKE_PKG_CONFIG=pkgs/fake-pkg-config packer
  run $fake link tools
  shim packer y
  shim packer --diagnose-tool-lookup
  # A config that changed: the table is written again.
  echo "-- touched" >>"$proj/korvid.config"
  touch -t 203001010000 "$proj/korvid.config"
  shim KORMAKE_PKG_CONFIG=pkgs/fake-pkg-config packer --diagnose-tool-lookup
  # A script the config publishes, run by the engine's kvscript (here a stub module, and a
  # stub that prints what it was given), and gen's note that it has no link, said once.
  mkdir -p "$root/samples/kvscript" "$proj/scripts"
  printf 'module "kvscript" { output = "exe", platforms = { "host" }, publish = true }\n' >"$root/samples/kvscript/module.lua"
  echo '-- makes an atlas' >"$proj/scripts/mkatlas.lua"
  echo 'command "mkatlas" { script = "scripts/mkatlas.lua" }' >>"$proj/korvid.config"
  stub "$proj/build/$host-dev/bin/kvscript" "kvscript"
  run $fake gen host-dev
  run $fake gen host-dev
  run $fake link tools mkatlas
  shim mkatlas --out atlas
  shim mkatlas --diagnose-tool-lookup
  # Out of tree, a default: the project's packer, until it is cleared.
  run $fake link default packer
  run $fake link default nosuch
  cwd=$lt/elsewhere shim packer z
  cwd=$lt/elsewhere shim packer --diagnose-tool-lookup
  run link default --clear packer
  cwd=$lt/elsewhere shim packer
  run link which stamp
  run link list
  # A newer shim installed stays: an older engine does not put its own over it.
  echo "9.0" >"$lt/home/kvbinlink.version"
  run $fake link tools packer
  run link remove packer
  run link remove nosuch
  cwd=$root
  run link remove --forget
  ls "$lt" >>"$out/$log.raw"
  normalize <"$out/$log.raw" | sed -e "s|$(re "$lt")|{lt}|g" -e "s|$(re "$(cd "$tmp" && pwd -P)")|{tmp}|g" |
    sed -E -e '/^  out:            ([{]tmp[}]|\/)/d' >"$out/$log"  # the walk past the test's directories is the machine's
  rm "$out/$log.raw"
  unset KORVID_LINK_HOME KORVID_LINK_BIN
  PATH=$keep_path
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
