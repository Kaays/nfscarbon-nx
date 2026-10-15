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

# mesa-switch's Docker script never tells rustc the target (only its MSYS2 script exports
# MESA_SWITCH_RUST_TARGET), so the Rust parts of NVK (nil, NAK) came out as x86-64 objects and the NRO
# link failed with "file in wrong format". Pass the target into every docker exec and add the
# aarch64-unknown-linux-gnu standard library to the image's Rust toolchain.
if ! grep -q MESA_SWITCH_RUST_TARGET "$MESA/build-switch.sh"; then
  echo "== fixing the Rust target in build-switch.sh"
  python3 - "$MESA/build-switch.sh" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = 'run() { docker exec "$CONTAINER_NAME" bash -c "$1"; }'
assert old in s, "build-switch.sh changed upstream"
s = s.replace(old, 'run() { docker exec -e MESA_SWITCH_RUST_TARGET=aarch64-unknown-linux-gnu "$CONTAINER_NAME" bash -c "$1"; }\n\n'
              '# Rust standard library for the Switch target (nfscarbon-nx).\n'
              "run '. /root/.cargo/env 2>/dev/null; rustup target add aarch64-unknown-linux-gnu'")
open(p, "w").write(s)
PY
fi

# With the target set, the wrapper also cross-compiled the crates Meson builds for the build machine
# (proc-macros such as paste and nak_ir_proc, and their dependencies syn, quote, proc-macro2...), which run
# inside the compiler and must stay x86-64. Meson calls both through the same "rustc" (the wrapper is first
# in PATH); the build-machine ones are the ones linked with the native "cc", the Switch ones with
# aarch64-none-elf-gcc. So the wrapper drops the target for native links and proc-macros.
if ! grep -q "nfscarbon-nx: native" "$MESA/rustc-switch-wrapper.sh"; then
  echo "== keeping build-machine Rust crates native in rustc-switch-wrapper.sh"
  python3 - "$MESA/rustc-switch-wrapper.sh" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = "        *sanity_check_for_rust.rs*|*sanitycheckrs.rs*) RUST_TARGET= ;;\n    esac\ndone\n"
assert old in s, "rustc-switch-wrapper.sh changed upstream"
s = s.replace(old, old + """
# nfscarbon-nx: native crates (build machine) keep the host target.
_cross=unknown
_prev=
for arg in "$@"; do
    case "$arg" in
        *aarch64-none-elf*) _cross=yes ;;
        linker=cc|linker=gcc|linker=c++|-Clinker=cc|-Clinker=gcc) [ "$_cross" = yes ] || _cross=no ;;
        proc-macro) [ "$_prev" = "--crate-type" ] && _cross=no ;;
    esac
    _prev=$arg
done
[ "$_cross" = no ] && RUST_TARGET=
""")
open(p, "w").write(s)
PY
fi

echo "== building (Docker)"
(cd "$MESA" && ./build-switch.sh)

LIB=$MESA/builddir-switch/src/nouveau/vulkan/libvulkan.a
[ -f "$LIB" ] || { echo "build finished but $LIB is missing"; exit 1; }
# Every object must be AArch64: an x86-64 one only shows up much later, when the NRO is linked.
OBJDUMP=${DEVKITPRO:-/opt/devkitpro}/devkitA64/bin/aarch64-none-elf-objdump
if [ -x "$OBJDUMP" ]; then
  wrong=$("$OBJDUMP" -f "$LIB" 2>/dev/null | grep "file format" | grep -vc "elf64-littleaarch64" || true)
  if [ "$wrong" != "0" ]; then
    echo "libvulkan.a has $wrong objects that are not AArch64:"
    "$OBJDUMP" -f "$LIB" 2>/dev/null | grep -B1 "file format" | grep -v "elf64-littleaarch64" | grep -v "^--" | head
    exit 1
  fi
  echo "== libvulkan.a: all objects are AArch64"
fi
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
