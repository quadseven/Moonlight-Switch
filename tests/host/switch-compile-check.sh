#!/usr/bin/env bash
#
# Cross-compile the Switch-only code paths locally.
#
#   ./switch-compile-check.sh [file.cpp ...]
#
# The host tests in this directory exercise the exporters' logic, but they
# compile with __SWITCH__ undefined, so everything inside those guards is
# invisible to them: the fsync that commits the journal to the card, the whole
# CPU exception handler, svcGetThreadId, armGetSystemTick. Those are exactly the
# parts that cannot be tested any other way, and they were reaching hardware
# having never been compiled anywhere but CI.
#
# Which was a problem the day GitHub Actions had an outage and two commits sat
# unverified with no way to check them. This removes that dependency: the same
# devkitPro image CI uses, run locally, compiling only the files that changed.
# Seconds rather than the ten minutes a full build takes, because it stops at
# the object file and never links.
#
# Requires a container runtime. colima works:  colima start
#
# This is a compile check, not a build. It proves the code is valid for the
# target and that symbols land as intended; it does not produce an NRO.

set -euo pipefail

cd "$(dirname "$0")/../.."

IMAGE="devkitpro/devkita64:latest"

# A Docker Desktop install that is no longer present can leave a credsStore
# pointing at a helper that does not exist, which fails every pull. Use a
# scratch config rather than editing the user's.
CFG=$(mktemp -d)
trap 'rm -rf "$CFG"' EXIT
python3 - "$CFG/config.json" <<'PY'
import json, os, sys
src = os.path.expanduser("~/.docker/config.json")
cfg = {}
if os.path.exists(src):
    try:
        cfg = json.load(open(src))
    except Exception:
        cfg = {}
    cfg.pop("credsStore", None)
    cfg.pop("credHelpers", None)
json.dump(cfg, open(sys.argv[1], "w"))
PY
export DOCKER_CONFIG="$CFG"

# The scratch config has no context registry, so name the endpoint directly
# rather than relying on the "colima" context being resolvable from it.
if [ -z "${DOCKER_HOST:-}" ] && [ -S "$HOME/.colima/default/docker.sock" ]; then
    export DOCKER_HOST="unix://$HOME/.colima/default/docker.sock"
fi

if ! docker info >/dev/null 2>&1; then
    echo "no container runtime. try: colima start" >&2
    exit 2
fi

# Only files that compile against libnx and the standard library alone.
#
# Anything pulling in borealis, ffmpeg or moonlight-common-c needs the full
# CMake configure to find its headers, which is the ten minute build this exists
# to avoid. Reporting those as FAILED for a missing include path would be a
# check that cries wolf, and a check nobody trusts is worse than no check.
#
# The list is deliberately explicit rather than a grep for __SWITCH__: it should
# grow when a file is written to be self-contained, not silently whenever one
# happens to mention the macro. Pass filenames to override.
if [ $# -gt 0 ]; then
    FILES="$*"
else
    FILES="app/src/utils/SwitchExceptionHandler.cpp \
           app/src/utils/OtlpTraceExporter.cpp \
           app/src/utils/OtlpMetricsExporter.cpp"
fi
echo "checking: $FILES"
echo

docker run --rm -v "$PWD:/src" -w /src "$IMAGE" bash -lc '
set -e
CXX=$DEVKITPRO/devkitA64/bin/aarch64-none-elf-g++
NM=$DEVKITPRO/devkitA64/bin/aarch64-none-elf-nm

# Matching what the real build uses. -mtp=soft matters: the thread pointer
# register is not readable from EL0 here, which is the same reason nothing in
# this app may use thread_local.
FLAGS="-std=gnu++20 -D__SWITCH__ -march=armv8-a+crc+crypto -mtune=cortex-a57 \
       -mtp=soft -ffunction-sections -fdata-sections \
       -isystem $DEVKITPRO/libnx/include -Iapp/src/utils \
       -Wall -Wextra -Wno-unused-parameter"

# -isystem, not -I: libnx headers trip -Wextra all by themselves and bury the
# warnings that belong to our code, which is the only reason to run -Wextra.

echo "compiler: $($CXX --version | head -1)"
echo
fail=0
for f in '"$FILES"'; do
    printf "  %-52s " "$f"
    if $CXX $FLAGS -c "$f" -o /tmp/obj.o 2>/tmp/err; then
        echo "OK"
    else
        echo "FAILED"
        sed "s/^/      /" /tmp/err | head -25
        fail=1
    fi
done

# The exception handler is only useful if it actually replaces libnx s weak
# definition. A C++ mangled name, or a static one, compiles perfectly and
# silently never runs, which would look identical to a crash that produced no
# fault. Worth asserting rather than assuming.
if echo "'"$FILES"'" | grep -q SwitchExceptionHandler; then
    echo
    $CXX $FLAGS -c app/src/utils/SwitchExceptionHandler.cpp -o /tmp/eh.o
    if $NM /tmp/eh.o | grep -q "^[0-9a-f]* T __libnx_exception_handler$"; then
        echo "  exception handler: defined strong and unmangled, overrides libnx"
    else
        echo "  exception handler: NOT a strong unmangled symbol, would not override"
        $NM -C /tmp/eh.o | grep -i exception | sed "s/^/      /"
        fail=1
    fi
fi

exit $fail
'
