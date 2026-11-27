#!/bin/bash
# Code generation and build for every edition whose game is extracted, one after the other, with a
# short report of what each edition got (hooks mapped, hook sources left out, PGO, build result).
#
#   tools/all_regions.sh /path/to/mesa-sdk/opt/devkitpro/portlibs/switch [jobs]
#   REGIONS="usa fr" tools/all_regions.sh ...   (only these editions)
#
# Order: usa first (the other editions find the USA hook addresses in its image), collector_pal last
# (app/generated is left with the last edition). An edition without assets/game_root_<region>/default.xex
# is skipped. PGO: an edition with .gcda files in pgo_<region>/ is built with them, the others without.
#
# Writes out/regions/report.txt, out/regions/<region>.log (codegen and build output) and
# out/regions/<region>/nfscarbon.nro.
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
MESA=${1:?usage: tools/all_regions.sh <mesa sdk>/opt/devkitpro/portlibs/switch [jobs]}
JOBS=${2:-4}
REGIONS=${REGIONS:-"usa pal2 pal fr it de es collector_usa collector_pal"}
DEST=$ROOT/out/regions
mkdir -p "$DEST"
REPORT=$DEST/report.txt
echo "nfscarbon-nx editions, $(date '+%Y-%m-%d %H:%M'), commit $(git -C "$ROOT" rev-parse --short HEAD)" > "$REPORT"

for r in $REGIONS; do
  log=$DEST/$r.log
  if [ ! -f "$ROOT/assets/game_root_$r/default.xex" ]; then
    echo "$r: skipped (no assets/game_root_$r/default.xex)" | tee -a "$REPORT"
    continue
  fi
  echo "== $r: codegen"
  if ! REGION=$r bash "$ROOT/tools/codegen.sh" > "$log" 2>&1; then
    echo "$r: CODEGEN FAILED (see out/regions/$r.log)" | tee -a "$REPORT"
    continue
  fi
  pgo=off
  ls "$ROOT"/pgo_$r/*.gcda >/dev/null 2>&1 && pgo=use
  echo "== $r: build (PGO $pgo)"
  if REGION=$r PGO=$pgo bash "$ROOT/tools/build.sh" "$MESA" "$JOBS" >> "$log" 2>&1; then
    mkdir -p "$DEST/$r"
    cp "$ROOT/app/out/switch-$r/nfscarbon.nro" "$DEST/$r/nfscarbon.nro"
    result="built"
  else
    result="BUILD FAILED (see out/regions/$r.log)"
  fi
  {
    echo "$r: $result, PGO $pgo"
    # map_edition.py and region_sources.py lines (none for usa, whose hooks need no mapping).
    grep -E "^mapped |hook sources translated|^  left out " "$log" | sed 's/^/    /'
  } | tee -a "$REPORT"
done
echo "report: $REPORT"
