# Host-side workflow. west runs inside the container.
# Windows: use the justfile in this directory (GNU make is not required).

.PHONY: help sync-env up down shell wipe wipe-volumes image rebuild init update build flash clean

COMPOSE := $(shell if docker compose version >/dev/null 2>&1; then echo "docker compose"; else echo "docker-compose"; fi)
export COMPOSE_PROJECT_NAME := $(shell echo $(notdir $(CURDIR)) | tr '[:upper:]' '[:lower:]' | tr -c 'a-z0-9_-' '-' | sed 's/^-//;s/-$$//')
WORKSPACE_NAME := $(notdir $(CURDIR))
WORKSPACE_IN_CONTAINER := /home/dev/$(WORKSPACE_NAME)
FACTORY_IMAGE := ghcr.io/mateuszzx/zephyr-workspace:latest
SERVICE := zephyr_env

BOARD ?= adi_sdp_k1/stm32f469xx

help:
	@echo "Project: $(COMPOSE_PROJECT_NAME)  (container: $(COMPOSE_PROJECT_NAME)-env)"
	@echo "Workspace: $(WORKSPACE_IN_CONTAINER)"
	@echo "Image: $(FACTORY_IMAGE)"
	@echo "BOARD=$(BOARD)"
	@echo "Targets: up down shell wipe wipe-volumes image rebuild init update build flash clean"
	@echo "Zephyr sources: /opt/zephyr-ws (container volume, not this repo)"
	@echo "Example: make up && make init && make build && make flash"

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

init: sync-env
	@if [ -f /.dockerenv ]; then \
		bash scripts/zephyr-ws.sh init; \
	else \
		$(COMPOSE) exec -u dev -w $(WORKSPACE_IN_CONTAINER) $(SERVICE) \
			zsh -ic 'bash scripts/zephyr-ws.sh init'; \
	fi

update: sync-env
	@if [ -f /.dockerenv ]; then \
		bash scripts/zephyr-ws.sh update; \
	else \
		$(COMPOSE) exec -u dev -w $(WORKSPACE_IN_CONTAINER) $(SERVICE) \
			zsh -ic 'bash scripts/zephyr-ws.sh update'; \
	fi

build: sync-env
	@if [ -f /.dockerenv ]; then \
		bash scripts/zephyr-ws.sh build $(BOARD); \
	else \
		$(COMPOSE) exec -u dev -w $(WORKSPACE_IN_CONTAINER) $(SERVICE) \
			zsh -ic 'bash scripts/zephyr-ws.sh build $(BOARD)'; \
	fi

flash: sync-env
	@if [ -f /.dockerenv ]; then \
		bash scripts/zephyr-ws.sh flash; \
	else \
		$(COMPOSE) exec -u root -w $(WORKSPACE_IN_CONTAINER) $(SERVICE) \
			zsh -ic 'bash scripts/zephyr-ws.sh flash'; \
	fi

clean:
	rm -rf build
