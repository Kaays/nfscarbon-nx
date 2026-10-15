#!/bin/bash
# Builds the NVK Vulkan driver for the Switch on Linux / WSL, with Docker, and leaves the folder the app needs
# (REXGLUE_SWITCH_NVK_SDK) at out/mesa-sdk.
#
#   tools/build_mesa_wsl.sh
#
# Steps:
#   1. clones danfromtico/mesa-switch into out/mesa-switch at commit 1a8c1a66d6f (the one nfsmw-nx uses);
#   2. applies mesa/mesa-switch-nfsmw.patch (nfsmw-nx's driver changes: ZCULL, cheaper draws, NAK fixes...);
#   3. runs mesa-switch's own build-switch.sh, which builds everything inside a Docker container
#      (image devkitpro-mesa-rust; the first time it downloads and builds the image, which takes a while);
#   4. copies the merged libvulkan.a (NVK, NAK and the rest of Mesa in one archive) to out/mesa-sdk/lib.
#
# Re-running it skips the clone and the patch and only rebuilds.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
MESA=$ROOT/out/mesa-switch
SDK=$ROOT/out/mesa-sdk
COMMIT=1a8c1a66d6f

command -v docker >/dev/null || { echo "docker not found: install Docker (Docker Desktop with WSL integration)"; exit 1; }
docker info >/dev/null 2>&1 || { echo "docker is installed but not running or not accessible (try: sudo service docker start)"; exit 1; }

mkdir -p "$ROOT/out"
if [ ! -d "$MESA/.git" ]; then
  echo "== cloning mesa-switch"
  git clone https://github.com/danfromtico/mesa-switch.git "$MESA"
  git -C "$MESA" checkout -q "$COMMIT"
fi
if ! git -C "$MESA" diff --quiet; then
  echo "== patch already applied"
else
  echo "== applying mesa/mesa-switch-nfsmw.patch"
  git -C "$MESA" apply "$ROOT/mesa/mesa-switch-nfsmw.patch"
  chmod +x "$MESA"/*.sh "$ROOT"/tools/*.sh
fi

echo "== building (Docker)"
(cd "$MESA" && ./build-switch.sh)

LIB=$MESA/builddir-switch/src/nouveau/vulkan/libvulkan.a
[ -f "$LIB" ] || { echo "build finished but $LIB is missing"; exit 1; }
mkdir -p "$SDK/lib"
cp "$LIB" "$SDK/lib/"

# The NRO also links expat and zstd (Mesa's dependencies). The build image already has them in its
# devkitPro portlibs, so they are taken from there: the host's devkitPro does not need them installed.
echo "== copying expat and zstd from the build image"
docker run --rm -v "$SDK/lib:/out" devkitpro-mesa-rust bash -c \
  'for l in expat zstd; do f=/opt/devkitpro/portlibs/switch/lib/lib$l.a; if [ -f $f ]; then cp $f /out/; echo "  lib$l.a"; else echo "  lib$l.a NOT in the image"; fi; done'

echo
echo "done: $SDK/lib/libvulkan.a ($(du -h "$SDK/lib/libvulkan.a" | cut -f1))"
echo "use it with: tools/build.sh $SDK"
