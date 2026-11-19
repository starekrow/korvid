#!/bin/sh
# Nothing runs before main (docs/principles.md, 2): no object of a build has a dynamic
# initializer. The module tables are constinit, so the compiler refuses one there; this
# catches the rest, a global or thread_local whose type the compiler can't initialize as a
# constant. The fix is `constinit` on the variable and an initializer on every member the
# type leaves without one. slang's CPU shaders are skipped: its prelude computes one
# constant at load time (g_f16tof32Magic), harmless, and not ours to edit.
#
#   tools/check-static-init.sh [build/linux-dev]
set -e
build=${1:-build/linux-dev}
[ -d "$build/obj" ] || { echo "check-static-init: no $build/obj; build first" >&2; exit 2; }
found=0
for o in $(find "$build/obj" -name '*.o' | sort); do
    case "$o" in *_shader_cpu.o) continue ;; esac
    if nm "$o" 2>/dev/null | grep -q '_GLOBAL__sub_I'; then
        echo "$o: has a dynamic initializer"
        found=1
    fi
done
if [ $found -ne 0 ]; then
    echo "check-static-init: something runs before main; see above" >&2
    exit 1
fi
echo "check-static-init: ok"
