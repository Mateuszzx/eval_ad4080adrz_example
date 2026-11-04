#!/usr/bin/env bash
# Zephyr and the STM32 HALs live in the container volume /opt/zephyr-ws.
# This repository keeps only the manifest (app/west.yml) and the application.
set -euo pipefail

WS=/opt/zephyr-ws
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BOARD="${2:-adi_sdp_k1/stm32f469xx}"

install_manifest() {
	mkdir -p "${WS}/app"
	cp "${ROOT}/app/west.yml" "${WS}/app/west.yml"
}

ensure_ws() {
	install_manifest
	if [[ -f "${WS}/.west/config" ]]; then
		return
	fi
	# First run after this repo used to keep the checkout in-tree.
	if [[ -d "${ROOT}/.west" && -d "${ROOT}/zephyr" ]]; then
		mv "${ROOT}/.west" "${WS}/.west"
		mv "${ROOT}/zephyr" "${WS}/zephyr"
		if [[ -d "${ROOT}/modules" ]]; then
			mv "${ROOT}/modules" "${WS}/modules"
		fi
		return
	fi
	west init -l "${WS}/app"
}

case "${1:-}" in
init)
	ensure_ws
	(cd "${WS}" && west update)
	;;
update)
	install_manifest
	(cd "${WS}" && west update)
	;;
build)
	(cd "${WS}" && west build -b "${BOARD}" -d "${ROOT}/build" "${ROOT}")
	;;
flash)
	(cd "${WS}" && west flash -d "${ROOT}/build")
	;;
*)
	echo "usage: zephyr-ws.sh init|update|build [board]|flash" >&2
	exit 1
	;;
esac
