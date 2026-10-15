#!/bin/bash
# Builds the code generator (rexglue) for your PC on Linux / WSL: sdk/out/linux-amd64/rexglue.
#
#   tools/build_rexglue_wsl.sh
#
# Needs Clang 20 or newer (the SDK uses C++23's std::expected; Clang 18 with libstdc++ 13 rejects it), CMake 3.25+,
# Ninja and the X11/Wayland development headers the SDK's runtime library is built with (the code generator links it):
#
#   wget https://apt.llvm.org/llvm.sh && sudo bash llvm.sh 20
#   sudo apt install cmake ninja-build pkg-config libx11-dev libx11-xcb-dev libxext-dev libxcursor-dev libxi-dev \
#        libxrandr-dev libxss-dev libxtst-dev libxfixes-dev libxkbcommon-dev libwayland-dev wayland-protocols
#
# Set CLANG / CLANGXX to use other compiler names.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CLANG=${CLANG:-clang-20}
CLANGXX=${CLANGXX:-clang++-20}
command -v "$CLANGXX" >/dev/null || { echo "$CLANGXX not found: install Clang 20 (see the top of this script)"; exit 1; }
[ -f "$ROOT/sdk/thirdparty/fmt/CMakeLists.txt" ] || { echo "run 'python3 tools/fetch_thirdparty.py' first"; exit 1; }

# -march=x86-64-v2: the SDK's byte swapping uses SSSE3/SSE4.1 (its CMakePresets.json sets the same).
cmake -S "$ROOT/sdk" -B "$ROOT/out/host" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$CLANG" -DCMAKE_CXX_COMPILER="$CLANGXX" \
  -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2 \
  -DREXGLUE_ENABLE_TRACY=OFF
cmake --build "$ROOT/out/host" --target rexglue
echo
echo "done: $ROOT/sdk/out/linux-amd64/rexglue"
echo "next: REXGLUE=$ROOT/sdk/out/linux-amd64/rexglue tools/codegen.sh"
