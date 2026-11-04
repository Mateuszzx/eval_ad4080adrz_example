# Zephyr RTOS project template

Pulls **`ghcr.io/mateuszzx/zephyr-workspace:latest`** (`zephyr-build:v0.29.4` SDK + uv + zsh/fzf).

West pins Zephyr **`v4.5.0-rc1`**. Example app: **blinky** (`led0` via GPIO). Workspace: **`/home/dev/<folder>`** as `dev` / zsh.

## Log in to GHCR

Images are published to `ghcr.io/mateuszzx/` and start out private. `make up` and `just up` fail with **401 Unauthorized** until Docker has logged in. The password is a personal access token, not your GitHub password.

1. Create a classic token: [github.com/settings/tokens/new](https://github.com/settings/tokens/new).
2. Enable **read:packages**. Add **write:packages** only when you publish images with `make push-*`.
3. Log in with your GitHub username in lowercase. Paste the token at the password prompt:

```bash
docker login ghcr.io -u mateuszzx
```

`denied: denied` means the password was not that token.

## Start

```bash
make zephyr          # from Project_Templates/ (once)
cd template-zephyr
make up && make shell
```

## Limit module downloads (vendor family)

By default west would clone **all** Zephyr HALs. This template uses `import.name-allowlist` so you only pull what you need:

| `FAMILY` | Modules pulled (besides Zephyr itself) |
|----------|----------------------------------------|
| `qemu` (default) | none — for `qemu_*` boards |
| `nrf` | `cmsis_6`, `hal_nordic`, `segger` |
| `stm32` | `cmsis_6`, `cmsis`, `hal_stm32`, `hal_st` |
| `esp` | `hal_espressif` |
| `full` | everything (large) |

```bash
make family FAMILY=nrf      # writes app/west.yml
make update                 # fetch only those modules
make build BOARD=nrf52840dk/nrf52840
west flash
```

Or in one step: `make update FAMILY=stm32`.

Presets live in [`app/manifests/`](app/manifests/). Edit a preset (or `app/west.yml`) to add modules (e.g. `mbedtls`, `lvgl`) when you need them.

If you already ran a **full** `west update`, unused repos stay on disk until you remove them; switching family + `make update` stops tracking extras for new clones. Clean re-init:

```bash
rm -rf .west zephyr modules bootloader build
make init FAMILY=nrf
```

## Workflow

```bash
make init FAMILY=nrf
make build BOARD=nrf52840dk/nrf52840
make flash                 # or: west flash
```

QEMU smoke test (no hardware):

```bash
make init FAMILY=qemu
make build BOARD=qemu_x86
```

App sources: [`app/src/main.c`](app/src/main.c) (Zephyr `samples/basic/blinky`).

## Flash tools (in the factory image)

| Family | Tools available for `west flash` |
|--------|-----------------------------------|
| `nrf` | **nrfutil** (+ `device`), **J-Link** |
| `stm32` | **OpenOCD** (Zephyr SDK hosttools + apt) |
| `esp` | **esptool** (already in zephyr-build) |

```bash
west flash                         # default runner for the board
west flash -r nrfutil              # Nordic DK
west flash -r openocd              # many STM32
west flash -r esptool              # ESP
```

Rebuild the image after tool changes: `make zephyr` from the repo root, then `make up` in the template.

## Cleanup

| Action | Effect |
|--------|--------|
| `make clean` | remove `build/` |
| `make down` | stop container; keep overlay + factory images |
| `make wipe` | stop + remove this project’s `*-overlay:local` image |
| `rm -rf .west zephyr modules bootloader build` | drop west checkouts (GB-scale) |

Factory GHCR images and Docker volumes: see [root README — Disk cleanup](../README.md#disk-cleanup).
