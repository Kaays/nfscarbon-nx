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
start=$(date +%s)
cmake --build "$OUT" -j "$JOBS"
echo "built $OUT/nfscarbon.nro in $(( ($(date +%s) - start) / 60 )) min"
