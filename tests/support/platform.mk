# Shared source selection for hardware-only guest Makefiles.
HARDWARE_PATHS_VALID := $(shell python3 "$(PROJECT_ROOT)/tools/hardware/hardware_paths.py" >/dev/null && echo valid)
ifneq ($(HARDWARE_PATHS_VALID),valid)
$(error Invalid hardware path configuration)
endif
GOLEM_LLVM_DIR ?= $(PROJECT_ROOT)/install/llvm
BUILD_ROOT ?= $(if $(GOLEM_BUILD_ROOT),$(GOLEM_BUILD_ROOT),$(PROJECT_ROOT)/build/src)
INSTALL_ROOT ?= $(if $(GOLEM_INSTALL_ROOT),$(GOLEM_INSTALL_ROOT),$(PROJECT_ROOT)/install/src)
PLATFORM_ROOT := $(PROJECT_ROOT)/src/platform/devices
PLATFORM_STARTUP_ROOT := $(PROJECT_ROOT)/src/platform/startup

# Fixed-address DMA fixtures own the low 64 KiB; code must not overlap them.
TEST_SPM_LINK_FLAGS := -Wl,--defsym,SPM_CODE_OFFSET=65536
