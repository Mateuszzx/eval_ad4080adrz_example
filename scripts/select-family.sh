#!/usr/bin/env bash
# Select which Zephyr HAL/modules west will clone.
# Usage: scripts/select-family.sh <qemu|nrf|stm32|esp|full>
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FAMILY="${1:-qemu}"
SRC="$ROOT/app/manifests/west-${FAMILY}.yml"
DST="$ROOT/app/west.yml"

if [[ ! -f "$SRC" ]]; then
  echo "Unknown FAMILY='$FAMILY'. Choose: qemu nrf stm32 esp full" >&2
  echo "Manifests in: app/manifests/" >&2
  exit 1
fi

cp "$SRC" "$DST"
echo "west.yml ← family=${FAMILY}  ($(basename "$SRC"))"
echo "Next: make update   # or make init if .west is missing"
