#!/bin/bash
# Builds app/out/switch/nfscarbon.nro (Linux / macOS / MSYS2).
#
#   tools/build.sh /path/to/mesa-sdk/opt/devkitpro/portlibs/switch [jobs]
#
# Needs devkitPro (DEVKITPRO, default /opt/devkitpro), CMake 3.25+, Ninja, the generated code
# (tools/codegen.sh) and the NVK SDK built with mesa/mesa-switch-nfsmw.patch (mesa/README.md).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
MESA=${1:?usage: tools/build.sh <mesa sdk>/opt/devkitpro/portlibs/switch [jobs]}
JOBS=${2:-4}
OUT=$ROOT/app/out/switch
if [ ! -f "$OUT/CMakeCache.txt" ]; then
  cmake -S "$ROOT/app" -B "$OUT" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/tools/switch/cmake/switch-devkitA64.cmake" \
    -DREXSDK_DIR="$ROOT/sdk" -DREXGLUE_SWITCH_NVK_SDK="$MESA"
fi
# PGO=generate | use | off (empty keeps what the build folder already has). See app/CMakeLists.txt.
if [ -n "${PGO:-}" ]; then
  [ "$PGO" = off ] && PGO=""
  if [ "$PGO" = use ] && ! ls "$ROOT"/pgo/*.gcda >/dev/null 2>&1; then
    echo "PGO=use: no .gcda files in $ROOT/pgo (copy them from sdmc:/switch/nfscarbon-nx/pgo)" >&2
    exit 1
  fi
  cmake -S "$ROOT/app" -B "$OUT" -DNFSCARBON_PGO="$PGO" >/dev/null
fi
start=$(date +%s)
cmake --build "$OUT" -j "$JOBS"
echo "built $OUT/nfscarbon.nro in $(( ($(date +%s) - start) / 60 )) min"
