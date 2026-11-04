# Host and container. On the host, recipes call Docker. Inside the container
# they run west directly.
# Host install: https://github.com/casey/just
# Example: just init family=nrf
#          just build board=nrf52840dk/nrf52840
#          just flash

workspace := file_name(justfile_directory())
project := replace_regex(replace_regex(lowercase(workspace), "[^a-z0-9_-]+", "-"), "^-+|-+$", "")
service := "zephyr_env"
root := "/home/dev/" + workspace
factory := "ghcr.io/mateuszzx/zephyr-workspace:latest"
inside := env_var_or_default("JUST_INSIDE_CONTAINER", "false")
host_only := "echo 'Run this on the host. Inside the container: just build, just flash, just shell.'"
copy := if os() == "windows" { "Copy-Item -Force" } else { "cp -f" }

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

# qemu | nrf | stm32 | esp | full
family family="qemu":
    {{ copy }} app/manifests/west-{{ family }}.yml app/west.yml

init family="qemu":
    just family {{ family }}
    {{ if inside == "true" { "west init -l app && west update" } else { "docker compose exec -u dev -w " + root + " " + service + " zsh -ic \"west init -l app && west update\"" } }}

update family="qemu":
    just family {{ family }}
    {{ if inside == "true" { "west update" } else { "docker compose exec -u dev -w " + root + " " + service + " zsh -ic \"west update\"" } }}

build board="qemu_x86":
    {{ if inside == "true" { "west build -b " + board + " -s app -d build" } else { "docker compose exec -u dev -w " + root + " " + service + " zsh -ic \"west build -b " + board + " -s app -d build\"" } }}

flash:
    {{ if inside == "true" { "west flash -d build" } else { "docker compose exec -u root -w " + root + " " + service + " zsh -ic \"west flash -d build\"" } }}

clean:
    {{ if os() == "windows" { "if (Test-Path build) { Remove-Item -Recurse -Force build }" } else { "rm -rf build" } }}
