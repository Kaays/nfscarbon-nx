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
# Output in app/out/linux-<region>/: nfscarbon, librexgpu-carbon.so, librexgpu-xenos.so, librexruntime.so and
# nfscarbon.toml (copied once). That folder is all a machine needs (with a Vulkan driver). Run it with
#   cd app/out/linux-<region> && ./nfscarbon --game_data_root=/path/to/game_root --gpu_plugin=carbon
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
JOBS=${1:-4}
REGION=${REGION:-usa}
OUT=$ROOT/app/out/linux-$REGION
# The first Clang 20 or newer found: clang++-22/21/20 (apt.llvm.org names), then plain clang++.
if [ -z "${CLANGXX:-}" ]; then
  for v in 22 21 20 ""; do
    cxx=clang++${v:+-$v}
    command -v "$cxx" >/dev/null || continue
    CLANGXX=$cxx
    CLANG=clang${v:+-$v}
    break
  done
fi
if [ -z "${CLANGXX:-}" ]; then
  echo "no clang++ found: install Clang 20 or newer, for example on Ubuntu / Kubuntu:" >&2
  echo "  wget https://apt.llvm.org/llvm.sh && sudo bash llvm.sh 20" >&2
  exit 1
fi
CLANG=${CLANG:-clang}
echo "compiler: $("$CLANGXX" --version | head -1)"
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
    -DREXGLUE_ENABLE_TRACY=OFF -DREXSDK_DIR="$ROOT/sdk" -DNFSCARBON_REGION="$REGION" \
    -DREXGLUE_BUILD_HOST_TOOLS=OFF
fi
# Without the code generator in the build, nothing re-runs codegen: app/generated stays what
# tools/codegen.sh made for the edition (also set on build folders configured before this option).
cmake -S "$ROOT/app" -B "$OUT" -DREXGLUE_BUILD_HOST_TOOLS=OFF >/dev/null
start=$(date +%s)
cmake --build "$OUT" -j "$JOBS"
# The SDK writes its shared runtime to sdk/out/linux-amd64; the executable looks next to itself ($ORIGIN).
cp -u "$ROOT/sdk/out/linux-amd64/librexruntime.so" "$OUT/"
[ -f "$OUT/nfscarbon.toml" ] || cp "$ROOT/app/nfscarbon.toml" "$OUT/"
echo "built in $(( $(date +%s) - start )) s: $OUT/nfscarbon"
echo "run:  cd $OUT && ./nfscarbon --game_data_root=<game_root folder> --gpu_plugin=carbon"
