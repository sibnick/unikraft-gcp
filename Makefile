# SPDX-License-Identifier: BSD-3-Clause
# Unikraft on Google Cloud Platform build and validation harness

.PHONY: all prepare build test test-host lint clean teardown

all: build

prepare:
	@./scripts/prepare-unikraft.sh

build: prepare test-host
	@$(MAKE) -C samples/helloworld defconfig
	@$(MAKE) -C samples/helloworld -j$$(nproc)
	@$(MAKE) -C samples/networkload defconfig
	@$(MAKE) -C samples/networkload -j$$(nproc)
	@$(MAKE) -C samples/httpreply defconfig
	@$(MAKE) -C samples/httpreply -j$$(nproc)

test-host:
	@cc -Wall -Wextra -Werror -std=c11 -Idrivers/gve/include tests/test_gve.c -o /tmp/test_gve
	@/tmp/test_gve
	@rm -f /tmp/test_gve
	@bash tests/test_evidence_store.sh
	@bash tests/test_gate_logic.sh

lint:
	@# Run from scripts/ so ShellCheck resolves the source= directives.
	@# Older hosts without ShellCheck skip instead of failing the build.
	@command -v shellcheck >/dev/null || { echo "lint: shellcheck not installed, skipping"; exit 0; }
	@cd scripts && shellcheck -x *.sh lib/*.sh && echo "lint: shellcheck clean"

test: build
	@# timeout 60 stops a unikernel that never powers off. Without it
	@# make test hangs forever. Ticket 4347c2a8e6 N6.
	@timeout 60 qemu-system-x86_64 -kernel samples/helloworld/build/helloworld_qemu-x86_64 -device virtio-net-pci -nographic

clean:
	@$(MAKE) -C samples/helloworld clean
	@rm -rf samples/helloworld/build
	@$(MAKE) -C samples/networkload clean
	@rm -rf samples/networkload/build
	@$(MAKE) -C samples/httpreply clean
	@rm -rf samples/httpreply/build

teardown:
	@# The run id is mandatory. A teardown with no run id deletes
	@# every ukg- resource in the project. Set UKG_TEARDOWN_ALL=1
	@# to allow that sweep. Ticket f3df948f2c.
	@if [ -z "$(RUN)" ] && [ "$${UKG_TEARDOWN_ALL:-0}" != "1" ]; then \
		echo "[ERROR] Missing run id." ; \
		echo "[ERROR] Usage: make teardown RUN=<run-id>" ; \
		echo "[ERROR] Example: make teardown RUN=ukg-1026-213005-4242" ; \
		echo "[ERROR] To sweep every ukg- resource on purpose, run:" ; \
		echo "[ERROR] UKG_TEARDOWN_ALL=1 make teardown" ; \
		exit 2 ; \
	fi
	@./scripts/gcp_teardown.sh $(RUN)
