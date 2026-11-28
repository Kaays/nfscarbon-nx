#!/bin/bash
# Desktop build for Linux x86-64 (not for release: to compare the game's behaviour with the Switch).
#
#   REGION=collector_pal tools/build_linux.sh [jobs]
#
# Same generated code as the Switch build (run tools/codegen.sh for the edition first). The renderer is the
# rexgpu-carbon shared library next to the executable, loaded through the SDK's GPU plugin seam, as in
# nfs-carbon-recomp. Needs what tools/build_rexglue_wsl.sh needs (Clang 20, CMake, Ninja, X11/Wayland
# headers) and a Vulkan driver (Mesa for Intel/AMD, or NVIDIA's).
#
# Output: app/out/linux-<region>/nfscarbon and librexgpu-carbon.so. Run it with
#   cp app/nfscarbon.toml app/out/linux-<region>/
#   app/out/linux-<region>/nfscarbon --game_data_root=/path/to/game_root --gpu_plugin=carbon
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
JOBS=${1:-4}
REGION=${REGION:-usa}
CLANG=${CLANG:-clang-20}
CLANGXX=${CLANGXX:-clang++-20}
OUT=$ROOT/app/out/linux-$REGION
command -v "$CLANGXX" >/dev/null || { echo "$CLANGXX not found: install Clang 20 (see tools/build_rexglue_wsl.sh)"; exit 1; }
GEN_REGION=$(cat "$ROOT/app/generated/REGION" 2>/dev/null || echo "?")
if [ "$GEN_REGION" != "$REGION" ]; then
  echo "app/generated is for '$GEN_REGION', not '$REGION': run REGION=$REGION bash tools/codegen.sh first" >&2
  exit 1
fi
if [ ! -f "$OUT/CMakeCache.txt" ]; then
  # -march=x86-64-v2: the SDK's byte swapping uses SSSE3/SSE4.1.
  cmake -S "$ROOT/app" -B "$OUT" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$CLANG" -DCMAKE_CXX_COMPILER="$CLANGXX" \
    -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2 \
    -DREXGLUE_ENABLE_TRACY=OFF -DREXSDK_DIR="$ROOT/sdk" -DNFSCARBON_REGION="$REGION"
fi
start=$(date +%s)
cmake --build "$OUT" -j "$JOBS"
echo "built in $(( $(date +%s) - start )) s: $OUT/nfscarbon"
