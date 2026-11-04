# Host and container. On the host, recipes call Docker. Inside the container
# they run west directly.
# Host install: https://github.com/casey/just
# Example: just init
#          just build
#          just flash
# `just build` uses adi_sdp_k1/stm32f469xx. Another board is positional.
# On just 1.21, "board=adi_sdp_k1/stm32f469xx" is taken as the board name.

workspace := file_name(justfile_directory())
project := replace_regex(replace_regex(lowercase(workspace), "[^a-z0-9_-]+", "-"), "^-+|-+$", "")
service := "zephyr_env"
root := "/home/dev/" + workspace
factory := "ghcr.io/mateuszzx/zephyr-workspace:latest"
inside := env_var_or_default("JUST_INSIDE_CONTAINER", "false")
host_only := "echo 'Run this on the host. Inside the container: just build, just flash, just shell.'"

export COMPOSE_PROJECT_NAME := project
export WORKSPACE_NAME := workspace
export COMPOSE_MENU := "0"

default:
    @just --list

up:
    {{ if inside == "true" { host_only } else { "docker compose up --detach --build" } }}
    {{ if inside == "true" { "true" } else { "echo 'Container " + project + "-env at " + root + "'" } }}
    {{ if inside == "true" { "true" } else { "echo 'Image: " + factory + "'" } }}

down:
    {{ if inside == "true" { host_only } else { "docker compose down" } }}

shell:
    {{ if inside == "true" { "exec zsh" } else { "docker compose exec -u dev -w " + root + " " + service + " zsh" } }}

wipe:
    {{ if inside == "true" { host_only } else { "docker compose down --rmi local" } }}

wipe-volumes:
    {{ if inside == "true" { host_only } else { "docker compose down --rmi local -v" } }}

rebuild:
    {{ if inside == "true" { host_only } else { "docker compose build --no-cache" } }}

init:
    {{ if inside == "true" { "bash scripts/zephyr-ws.sh init" } else { "docker compose exec -u dev -w " + root + " " + service + " zsh -ic \"bash scripts/zephyr-ws.sh init\"" } }}

update:
    {{ if inside == "true" { "bash scripts/zephyr-ws.sh update" } else { "docker compose exec -u dev -w " + root + " " + service + " zsh -ic \"bash scripts/zephyr-ws.sh update\"" } }}

build board="adi_sdp_k1/stm32f469xx":
    {{ if inside == "true" { "bash scripts/zephyr-ws.sh build " + board } else { "docker compose exec -u dev -w " + root + " " + service + " zsh -ic \"bash scripts/zephyr-ws.sh build " + board + "\"" } }}

flash:
    {{ if inside == "true" { "bash scripts/zephyr-ws.sh flash" } else { "docker compose exec -u root -w " + root + " " + service + " zsh -ic \"bash scripts/zephyr-ws.sh flash\"" } }}

clean:
    {{ if os() == "windows" { "if (Test-Path build) { Remove-Item -Recurse -Force build }" } else { "rm -rf build" } }}
