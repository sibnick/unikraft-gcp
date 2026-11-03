UK_ROOT ?= $(realpath $(CURDIR)/../../.unikraft/unikraft)
UK_LIBS ?= $(realpath $(CURDIR)/../../.unikraft/libs)
LIBS ?= $(UK_LIBS)/lwip:$(UK_LIBS)/libelf:$(UK_LIBS)/app-elfloader:$(realpath $(CURDIR)/../../drivers/gve)

.PHONY: all clean defconfig menuconfig

all:
	@$(MAKE) -C $(UK_ROOT) A=$(CURDIR) L=$(LIBS)

defconfig:
	@$(MAKE) -C $(UK_ROOT) A=$(CURDIR) L=$(LIBS) defconfig UK_DEFCONFIG=$(CURDIR)/defconfig

menuconfig:
	@$(MAKE) -C $(UK_ROOT) A=$(CURDIR) L=$(LIBS) menuconfig

clean:
	@if [ -d "$(UK_ROOT)" ]; then $(MAKE) -C $(UK_ROOT) A=$(CURDIR) L=$(LIBS) clean; fi
	@rm -rf build

ifneq (,$(filter-out all clean defconfig menuconfig,$(MAKECMDGOALS)))
$(filter-out all clean defconfig menuconfig,$(MAKECMDGOALS)):
	@$(MAKE) -C $(UK_ROOT) A=$(CURDIR) L=$(LIBS) $(filter-out all clean defconfig menuconfig,$(MAKECMDGOALS))
endif
