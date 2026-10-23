# SPDX-License-Identifier: BSD-3-Clause
# Unikraft on Google Cloud Platform build and validation harness

.PHONY: all prepare build test clean teardown

all: build

prepare:
	@./scripts/prepare-unikraft.sh

build: prepare
	@$(MAKE) -C samples/helloworld defconfig
	@$(MAKE) -C samples/helloworld -j$$(nproc)

test: build
	@qemu-system-x86_64 -kernel samples/helloworld/build/helloworld_qemu-x86_64 -device virtio-net-pci -nographic

clean:
	@$(MAKE) -C samples/helloworld clean
	@rm -rf samples/helloworld/build

teardown:
	@./scripts/gcp_teardown.sh
