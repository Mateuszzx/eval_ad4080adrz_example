#!/usr/bin/env bash
# Fix Docker volume ownership, then keep the container alive as "dev".
set -euo pipefail

DEV_USER="${USERNAME:-dev}"
DEV_HOME="$(getent passwd "$DEV_USER" | cut -d: -f6)"
DEV_HOME="${DEV_HOME:-/home/dev}"

for d in "${DEV_HOME}/.cursor-server" "${DEV_HOME}/.vscode-server"; do
  mkdir -p "$d"
  chown -R "${DEV_USER}:${DEV_USER}" "$d" || true
done
chown "${DEV_USER}:${DEV_USER}" "${DEV_HOME}" || true

exec su -s /bin/bash "${DEV_USER}" -c "exec sleep infinity"
