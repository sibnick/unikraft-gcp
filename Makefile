# SPDX-License-Identifier: BSD-3-Clause
# Unikraft on Google Cloud Platform build and validation harness

.PHONY: all prepare build test test-host clean teardown

all: build

prepare:
	@./scripts/prepare-unikraft.sh

build: prepare test-host
	@$(MAKE) -C samples/helloworld defconfig
	@$(MAKE) -C samples/helloworld -j$$(nproc)
	@$(MAKE) -C samples/networkload defconfig
	@$(MAKE) -C samples/networkload -j$$(nproc)

test-host:
	@cc -Wall -Wextra -Werror -std=c11 -Idrivers/gve/include tests/test_gve.c -o /tmp/test_gve
	@/tmp/test_gve
	@rm -f /tmp/test_gve

test: build
	@qemu-system-x86_64 -kernel samples/helloworld/build/helloworld_qemu-x86_64 -device virtio-net-pci -nographic

clean:
	@$(MAKE) -C samples/helloworld clean
	@rm -rf samples/helloworld/build
	@$(MAKE) -C samples/networkload clean
	@rm -rf samples/networkload/build

teardown:
	@./scripts/gcp_teardown.sh
