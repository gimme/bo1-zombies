# Makefile - build the engine: `make` lists the ways. Each is a docker/ script, which does the work and takes more
# options.
#
# Both builds compile the engine, and a client joins only a server of its own build (build_id.cmake), so make build runs
# the two every time and leaves it to Ninja, in each build's container, to compile only what changed.
#
# ARGS goes to the script a target ends in: make exe ARGS=--clean.

ARGS ?=
.DEFAULT_GOAL := help
.PHONY: help build exe web id package clean
# one at a time: each build takes every core anyway
.NOTPARALLEL:

help: ## this list
	@awk -F':.*## ' '/^[a-z-]+:.*## /{printf "  make %-8s %s\n", $$1, $$2}' $(MAKEFILE_LIST)

build: exe web ## make exe and make web

exe: ## build BO1Zombies.exe, the server's engine and the Windows client, into build/exe/Release (docker/build-exe.sh)
	docker/build-exe.sh $(ARGS)

web: ## build the web client into build/web (docker/build-web.sh)
	docker/build-web.sh $(ARGS)

id: ## show the build ID of the source and of each build: a client joins only a server of its own
	@docker/build-id.sh build/exe/Release/BO1Zombies.exe build/web/bo1zombies.wasm || \
		echo "different builds: make build fixes it"

package: ## pack the builds into build/bo1-zombies-<id>.tar.gz, the archive a zombies-hub server runs (docker/package.sh)
	docker/package.sh $(ARGS)

clean: ## delete the builds, build/exe and build/web, and keep the rest of build/
	rm -rf build/exe build/web build/web-asan
