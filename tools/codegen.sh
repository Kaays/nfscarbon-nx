#!/bin/bash
# Translates Carbon's default.xex to C++ (app/generated/default) and makes the calls between
# unhooked game functions direct, which LTO needs to inline across files.
#
#   REXGLUE=out/host/rexglue tools/codegen.sh
#
# Needs the game extracted to assets/game_root (tools/fase1_extraer.py) and the code generator built
# from sdk/ for your PC (README.md, step 4). Set PYTHON if Python is not "python3".
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
APP=$ROOT/app
REXGLUE=${REXGLUE:?set REXGLUE to the rexglue executable built from sdk/ for your PC}
PY=${PYTHON:-python3}
[ -f "$ROOT/assets/game_root/default.xex" ] || { echo "assets/game_root/default.xex not found: extract the game first"; exit 1; }
cd "$APP"
echo "== codegen"
"$REXGLUE" codegen nfscarbon_manifest.toml > codegen.log 2>&1 || { echo "codegen failed, see app/codegen.log"; exit 1; }
echo "== direct calls"
"$PY" "$ROOT/tools/direct_calls.py"
echo "done: app/generated/default"
