#!/bin/bash
# Zips each edition's NRO with the shipped nfscarbon.toml for a GitHub release:
# out/regions/nfscarbon-nx-<region>.zip from out/regions/<region>/nfscarbon.nro (tools/all_regions.sh).
#
#   tools/package_release.sh                  (every edition that has an NRO there)
#   REGIONS="usa fr" tools/package_release.sh
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEST=$ROOT/out/regions
REGIONS=${REGIONS:-"usa pal2 pal fr it de es collector_usa collector_pal"}
for r in $REGIONS; do
  nro=$DEST/$r/nfscarbon.nro
  if [ ! -f "$nro" ]; then
    echo "$r: no $nro, skipped"
    continue
  fi
  d=$(mktemp -d)
  cp "$nro" "$ROOT/app/nfscarbon.toml" "$d"/
  (cd "$d" && zip -q "nfscarbon-nx-$r.zip" nfscarbon.nro nfscarbon.toml)
  mv "$d/nfscarbon-nx-$r.zip" "$DEST/"
  rm -rf "$d"
  echo "$r: out/regions/nfscarbon-nx-$r.zip"
done
