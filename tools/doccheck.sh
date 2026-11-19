#!/usr/bin/env bash
# The documentation check (DOCSTYLE.md): the parts of the conventions a script can hold
# the docs to. Run from anywhere in the repository; it reads, never writes.
#
#   tools/doccheck.sh                    every document's links and anchors, and every
#                                        module's quick reference against its code
#   tools/doccheck.sh --module NAME      also NAME's public names missing from its quick
#                                        reference, and the template sections it lacks
#   tools/doccheck.sh --all              the same report for every module
#
# Errors (exit 1): a relative link to a file that does not exist, a link to a heading a
# page does not have, a name in a quick-reference table that appears nowhere in the
# module's include/ or src/, a "Header `korvid/...`" line naming a header that does not
# exist. Warnings (reported, exit 0): what --module and --all add, and modules without a
# README.
#
# The name check is a tripwire, not a proof: it looks for the last part of each name as
# a whole word in the module's code, so a renamed or removed function is caught, and a
# name that is still a word somewhere is not.
set -u

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root" || exit 2

report=()
all=0
while [ $# -gt 0 ]; do
    case "$1" in
        --module) [ $# -ge 2 ] || { echo "doccheck: --module takes a module name" >&2; exit 2; }
                  [ -d "modules/$2" ] || { echo "doccheck: no module '$2' in modules/" >&2; exit 2; }
                  report+=("$2"); shift 2 ;;
        --all)    all=1; shift ;;
        -h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)        echo "doccheck: unknown argument '$1' (--module NAME, --all)" >&2; exit 2 ;;
    esac
done
if [ $all -eq 1 ]; then
    report=()
    for d in modules/*/; do report+=("$(basename "$d")"); done
fi

errors=0

# ---- links and anchors ---------------------------------------------------------------
# Every [text](target) outside code: a relative target must exist, and a #fragment must
# be a heading of the target page (GitHub's slugs: lowercase, punctuation dropped, spaces
# to hyphens, a repeated heading numbered -1, -2, ...).
docs=$(find . -name '*.md' -not -path './build/*' -not -path '*/vendor/*' -not -path './.git/*' \
       -not -path '*/node_modules/*' | sed 's#^\./##' | sort)

# shellcheck disable=SC2086
awk '
function norm(p,   n, parts, out, i, k, s) {
    n = split(p, parts, "/"); k = 0
    for (i = 1; i <= n; i++) {
        if (parts[i] == "" || parts[i] == ".") continue
        if (parts[i] == "..") { if (k > 0 && out[k] != "..") k--; else out[++k] = ".."; continue }
        out[++k] = parts[i]
    }
    s = ""
    for (i = 1; i <= k; i++) s = s (i > 1 ? "/" : "") out[i]
    return s == "" ? "." : s
}
function dirof(p) { if (p ~ /\//) { sub(/\/[^\/]*$/, "", p); return p } return "." }
function slug(h) {
    h = tolower(h)
    gsub(/[^a-z0-9 _-]/, "", h)
    gsub(/ /, "-", h)
    return h
}
function load_anchors(f,   line, fence, s, c) {
    if (f in loaded) return
    loaded[f] = 1
    fence = 0
    while ((getline line < f) > 0) {
        if (line ~ /^[ \t]*```/) { fence = !fence; continue }
        if (fence || line !~ /^#+ /) continue
        sub(/^#+ +/, "", line)
        sub(/ +#+ *$/, "", line)
        s = slug(line)
        c = count[f, s]++
        if (c) s = s "-" c
        anchor[f, s] = 1
    }
    close(f)
}
FNR == 1 { fence = 0; dir = dirof(FILENAME) }
/^[ \t]*```/ { fence = !fence; next }
fence { next }
{
    line = $0
    gsub(/``[^`]*``/, "", line)
    gsub(/`[^`]*`/, "", line)
    while (match(line, /\]\([^) ]+\)/)) {
        target = substr(line, RSTART + 2, RLENGTH - 3)
        line = substr(line, RSTART + RLENGTH)
        if (target ~ /^[a-z]+:/) continue
        path = target; frag = ""
        h = index(path, "#")
        if (h) { frag = substr(path, h + 1); path = substr(path, 1, h - 1) }
        file = (path == "") ? norm(FILENAME) : norm(dir "/" path)
        if (path != "" && system("test -e \"" file "\"") != 0) {
            printf "%s:%d: broken link: %s\n", FILENAME, FNR, target
            bad++
            continue
        }
        if (frag != "" && file ~ /\.md$/) {
            load_anchors(file)
            if (!((file, frag) in anchor)) {
                printf "%s:%d: no heading #%s in %s\n", FILENAME, FNR, frag, file
                bad++
            }
        }
    }
}
END { exit bad > 0 }
' $docs || errors=1

# ---- quick references -----------------------------------------------------------------
# Every backticked name in the first column of a table under "## Quick reference" must
# appear in the module's code; every "Header `korvid/...`" must exist.
qr_names() { # README -> lines of "line<TAB>as written<TAB>name to look for"
    awk '
    /^## / { inqr = ($0 ~ /^## Quick reference/); next }
    inqr && /^\| *`/ {
        cell = $0
        gsub(/\\\|/, "", cell)
        sub(/^\| */, "", cell)
        sub(/ *\|.*$/, "", cell)
        while (match(cell, /`[^`]+`/)) {
            tok = substr(cell, RSTART + 1, RLENGTH - 2)
            cell = substr(cell, RSTART + RLENGTH)
            name = tok
            sub(/^[*^.?]+/, "", name)
            if (!match(name, /^[A-Za-z_][A-Za-z0-9_.:]*/)) continue
            name = substr(name, 1, RLENGTH)
            n = split(name, seg, /[.:]+/)
            name = seg[n]
            if (name != "") printf "%d\t%s\t%s\n", FNR, tok, name
        }
    }' "$1"
}
qr_headers() { # README -> lines of "line<TAB>korvid/x/y.h"
    awk '/^## / { inqr = ($0 ~ /^## Quick reference/); next }
         inqr && /^Headers? `korvid\// {
             line = $0
             while (match(line, /`korvid\/[^`]+\.h`/)) {
                 printf "%d\t%s\n", FNR, substr(line, RSTART + 1, RLENGTH - 2)
                 line = substr(line, RSTART + RLENGTH)
             }
         }' "$1"
}

no_readme=()
for d in modules/*/; do
    m=$(basename "$d")
    readme="modules/$m/README.md"
    if [ ! -f "$readme" ]; then no_readme+=("$m"); continue; fi
    code=()
    [ -d "modules/$m/include" ] && code+=("modules/$m/include")
    [ -d "modules/$m/src" ] && code+=("modules/$m/src")
    [ -d "modules/$m/assets" ] && code+=("modules/$m/assets") # markup and scripts the module ships (Krome's widgets)
    [ ${#code[@]} -gt 0 ] || continue
    while IFS=$'\t' read -r line tok name; do
        [ -n "$name" ] || continue
        if ! grep -rqwF -- "$name" "${code[@]}" 2>/dev/null; then
            echo "$readme:$line: quick reference names \`$tok\`, which is not in the module's code"
            errors=1
        fi
    done < <(qr_names "$readme")
    while IFS=$'\t' read -r line header; do
        found=0
        for inc in modules/*/include; do [ -f "$inc/$header" ] && { found=1; break; }; done
        if [ $found -eq 0 ]; then
            echo "$readme:$line: no header $header"
            errors=1
        fi
    done < <(qr_headers "$readme")
done

# ---- per-module reports (warnings) ------------------------------------------------------
# Public names: functions, structs and enums declared at the top level of the module's
# headers (this codebase does not indent namespace contents), not mentioned in its README.
# A section rule that starts "// ---- internal" leaves out what follows it, up to the next
# section rule.
public_names() { # header -> names, one per line
    awk '
    /^\/\/ ---- / { internal = ($0 ~ /^\/\/ ---- internal/); next }
    internal { next }
    /^[ \t]*\/\*/ { cm = 1 } cm { if (/\*\//) cm = 0; next }
    /^(\/\/|#|}|\{|namespace|using|typedef|template|static_assert|extern "C"|KV_|[ \t])/ { next }
    /^(struct|enum( class)?|union) [A-Za-z_][A-Za-z0-9_]* *(:[^;{]*)?\{/ {
        s = $0; sub(/^(struct|enum class|enum|union) /, "", s); sub(/[^A-Za-z0-9_].*$/, "", s); print s; next
    }
    /^(struct|enum( class)?|union) [A-Za-z_][A-Za-z0-9_]* *$/ {
        s = $0; sub(/^(struct|enum class|enum|union) /, "", s); sub(/[^A-Za-z0-9_].*$/, "", s); print s; next
    }
    {
        if (match($0, /[ *&][A-Za-z_][A-Za-z0-9_]*\(/)) {
            s = substr($0, RSTART + 1, RLENGTH - 2)
            if (s != "operator" && s != "sizeof" && s != "alignof" && s != "decltype") print s
        }
    }' "$1"
}

sections=("How it works" "Using it" "Quick reference" "Behaviour worth knowing" "Not in this module" "Seen in" "See also")
for m in ${report[@]+"${report[@]}"}; do
    readme="modules/$m/README.md"
    echo "== $m"
    if [ ! -f "$readme" ]; then echo "  warning: no README.md"; continue; fi
    for s in "${sections[@]}"; do
        grep -q "^## $s\$" "$readme" || echo "  warning: no '## $s' section"
    done
    missing=()
    if [ -d "modules/$m/include" ]; then
        while read -r name; do
            [ -n "$name" ] || continue
            grep -qwF -- "$name" "$readme" || missing+=("$name")
        done < <(find "modules/$m/include" -name '*.h' -print | sort | while read -r h; do public_names "$h"; done | sort -u)
    fi
    if [ ${#missing[@]} -gt 0 ]; then
        echo "  warning: ${#missing[@]} public name(s) in the headers, not in the README:"
        printf '    %s\n' "${missing[@]}"
    fi
done

if [ ${#no_readme[@]} -gt 0 ]; then
    echo "note: ${#no_readme[@]} module(s) without a README: ${no_readme[*]}"
fi
if [ $errors -ne 0 ]; then
    echo "doccheck: errors above" >&2
    exit 1
fi
echo "doccheck: ok"
