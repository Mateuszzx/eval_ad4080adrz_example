# Host-side workflow. west runs inside the container.
# Windows: use the justfile in this directory (GNU make is not required).

.PHONY: help sync-env up down shell wipe wipe-volumes image rebuild family init update build flash clean

COMPOSE := $(shell if docker compose version >/dev/null 2>&1; then echo "docker compose"; else echo "docker-compose"; fi)
export COMPOSE_PROJECT_NAME := $(shell echo $(notdir $(CURDIR)) | tr '[:upper:]' '[:lower:]' | tr -c 'a-z0-9_-' '-' | sed 's/^-//;s/-$$//')
WORKSPACE_NAME := $(notdir $(CURDIR))
WORKSPACE_IN_CONTAINER := /home/dev/$(WORKSPACE_NAME)
FACTORY_IMAGE := ghcr.io/mateuszzx/zephyr-workspace:latest
SERVICE := zephyr_env

BOARD ?= qemu_x86
APP := app
# Vendor HAL filter for west: qemu | nrf | stm32 | esp | full
FAMILY ?= qemu

help:
	@echo "Project: $(COMPOSE_PROJECT_NAME)  (container: $(COMPOSE_PROJECT_NAME)-env)"
	@echo "Workspace: $(WORKSPACE_IN_CONTAINER)"
	@echo "Image: $(FACTORY_IMAGE)"
	@echo "FAMILY=$(FAMILY)  BOARD=$(BOARD)"
	@echo "Targets: up down shell wipe wipe-volumes image rebuild family init update build flash clean"
	@echo "Example: make init FAMILY=nrf && make build BOARD=nrf52840dk/nrf52840 && make flash"
	@echo "Windows: just init family=nrf && just build board=nrf52840dk/nrf52840"

sync-env:
	@bash scripts/sync-compose-env.sh

image: sync-env
	@if [ "$(PULL)" = "1" ]; then docker pull $(FACTORY_IMAGE) || true; fi
	$(COMPOSE) build

rebuild: sync-env
	@if [ "$(PULL)" = "1" ]; then docker pull $(FACTORY_IMAGE) || true; fi
	$(COMPOSE) build --no-cache

up: sync-env
	@if [ "$(PULL)" = "1" ]; then docker pull $(FACTORY_IMAGE) || true; fi
	COMPOSE_MENU=0 $(COMPOSE) up --detach --build
	@echo "Container '$(COMPOSE_PROJECT_NAME)-env' at $(WORKSPACE_IN_CONTAINER) (user: dev / zsh)"

down: sync-env
	$(COMPOSE) down

wipe: sync-env
	$(COMPOSE) down --rmi local
	@echo "Removed container and $(COMPOSE_PROJECT_NAME)-overlay:local (factory image kept)"

wipe-volumes: sync-env
	$(COMPOSE) down --rmi local -v
	@echo "Removed container, overlay, and compose volumes for $(COMPOSE_PROJECT_NAME)"

shell: sync-env
	$(COMPOSE) exec -u dev -w $(WORKSPACE_IN_CONTAINER) \
		-e PROJECT_ROOT=$(WORKSPACE_IN_CONTAINER) -e WORKSPACE_NAME=$(WORKSPACE_NAME) \
		-e HOST=$(COMPOSE_PROJECT_NAME) $(SERVICE) zsh

family:
	@bash scripts/select-family.sh $(FAMILY)

init: sync-env family
	@if [ -f /.dockerenv ]; then \
		west init -l $(APP) && west update; \
	else \
		$(COMPOSE) exec -u dev -w $(WORKSPACE_IN_CONTAINER) $(SERVICE) \
			zsh -ic 'west init -l $(APP) && west update'; \
	fi

update: sync-env family
	@if [ -f /.dockerenv ]; then \
		west update; \
	else \
		$(COMPOSE) exec -u dev -w $(WORKSPACE_IN_CONTAINER) $(SERVICE) zsh -ic 'west update'; \
	fi

build: sync-env
	@if [ -f /.dockerenv ]; then \
		west build -b $(BOARD) -s $(APP) -d build; \
	else \
		$(COMPOSE) exec -u dev -w $(WORKSPACE_IN_CONTAINER) $(SERVICE) \
			zsh -ic 'west build -b $(BOARD) -s $(APP) -d build'; \
	fi

flash: sync-env
	@if [ -f /.dockerenv ]; then \
		west flash -d build; \
	else \
		$(COMPOSE) exec -u root -w $(WORKSPACE_IN_CONTAINER) $(SERVICE) \
			zsh -ic 'west flash -d build'; \
	fi

clean:
	rm -rf build
