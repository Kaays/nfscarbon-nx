#!/bin/bash
# Translates Carbon's default.xex to C++ (app/generated/default) and makes the calls between
# unhooked game functions direct, which LTO needs to inline across files.
#
#   tools/codegen.sh            (REXGLUE defaults to sdk/out/linux-amd64/rexglue)
#   REGION=pal tools/codegen.sh (edition; default usa)
#
# Needs the game extracted to assets/game_root_<region> (tools/fase1_extraer.py) and the code generator built
# from sdk/ for your PC (README.md, step 4). Set PYTHON if Python is not "python3".
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
APP=$ROOT/app
REXGLUE=${REXGLUE:-$ROOT/sdk/out/linux-amd64/rexglue}
[ -x "$REXGLUE" ] || { echo "rexglue not found at $REXGLUE: run tools/build_rexglue_wsl.sh first"; exit 1; }
PY=${PYTHON:-python3}
REGION=${REGION:-usa}
GAME=assets/game_root_$REGION
[ -f "$ROOT/$GAME/default.xex" ] || { echo "$GAME/default.xex not found: extract the game first"; exit 1; }
MANIFEST=nfscarbon_manifest.toml
[ -f "$APP/nfscarbon_manifest_$REGION.toml" ] && MANIFEST=nfscarbon_manifest_$REGION.toml
cd "$APP"
# Start from an empty folder. The code generator skips files whose inputs did not change, and
# direct_calls.py only rewrites calls that are not direct yet: with the old files kept, a function hooked
# since the last run would stay called as __imp__sub_X and its hook would be silently bypassed.
rm -rf generated/default
# The manifest reads assets/game_root: make it a link to this edition's folder.
if [ -e "$ROOT/assets/game_root" ] && [ ! -L "$ROOT/assets/game_root" ]; then
  echo "assets/game_root is a real folder: rename it to game_root_usa (or the edition it holds)"; exit 1
fi
ln -sfn "game_root_$REGION" "$ROOT/assets/game_root"
echo "== codegen ($REGION: $GAME, $MANIFEST)"
"$REXGLUE" codegen "$MANIFEST" > codegen.log 2>&1 || { echo "codegen failed, see app/codegen.log"; exit 1; }
echo "== direct calls"
REGION=$REGION "$PY" "$ROOT/tools/direct_calls.py"
echo "$REGION" > generated/REGION
echo "done: app/generated/default ($REGION)"
