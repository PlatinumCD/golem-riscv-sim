TEST_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
PROJECT_ROOT := $(abspath $(TEST_DIR)/../../..)
include $(PROJECT_ROOT)/tests/support/platform.mk

LLVM ?= $(if $(GOLEM_LLVM_DIR),$(GOLEM_LLVM_DIR),$(INSTALL_ROOT)/llvm)
TARGET ?= riscv64-unknown-elf
CPU ?= golem-analog
ABI ?= lp64d
CLANG ?= $(LLVM)/bin/clang
CLANGXX ?= $(LLVM)/bin/clang++
STARTUP_LD ?= $(PLATFORM_STARTUP_ROOT)/scratchpad.ld
OUT := $(if $(GOLEM_TEST_RESULTS_ROOT),$(GOLEM_TEST_RESULTS_ROOT),$(PROJECT_ROOT)/tests/results)/scratchpad-icache/guest

COMMON := --target=$(TARGET) -mcpu=$(CPU) -mabi=$(ABI) -mcmodel=medany \
          -ffreestanding -fno-stack-protector -fdata-sections -O2 -g
TESTS := warm-loop eviction straddle fence-i trap-ram rvv-spm rvv-fault reload-code dma-code

.PHONY: all clean
.SECONDARY:
all: $(TESTS:%=$(OUT)/%.elf)

$(OUT):
	mkdir -p -- $@

$(OUT)/crt0.o: $(PLATFORM_STARTUP_ROOT)/crt0.S | $(OUT)
	$(CLANG) $(COMMON) -c $< -o $@

$(OUT)/uart.o: $(PLATFORM_ROOT)/uart.cpp | $(OUT)
	$(CLANGXX) $(COMMON) -std=c++20 -fno-exceptions -fno-rtti -fno-use-cxa-atexit \
		-I$(PLATFORM_ROOT) -c $< -o $@

$(OUT)/exit.o: $(PLATFORM_STARTUP_ROOT)/platform-exit.cpp | $(OUT)
	$(CLANGXX) $(COMMON) -std=c++20 -fno-exceptions -fno-rtti -fno-use-cxa-atexit \
		-I$(PLATFORM_ROOT) -c $< -o $@

$(OUT)/%.o: $(TEST_DIR)/%.S $(TEST_DIR)/common.S | $(OUT)
	$(CLANG) $(COMMON) -mno-relax -c $< -o $@

$(OUT)/%.o: $(TEST_DIR)/%.cpp | $(OUT)
	$(CLANGXX) $(COMMON) -std=c++20 -fno-exceptions -fno-rtti \
		-I$(PLATFORM_ROOT) -c $< -o $@

$(OUT)/%.elf: $(OUT)/%.o $(OUT)/crt0.o $(OUT)/uart.o $(OUT)/exit.o $(STARTUP_LD)
	$(CLANGXX) $(COMMON) -nostdlib -nostartfiles -nodefaultlibs -fuse-ld=lld \
		-Wl,--build-id=none -Wl,--gc-sections -Wl,-T,$(STARTUP_LD) \
		$(OUT)/crt0.o $(OUT)/uart.o $(OUT)/exit.o $< -o $@
	@echo "built $@"

clean:
	rm -f -- $(TESTS:%=$(OUT)/%.elf) $(TESTS:%=$(OUT)/%.o) \
		$(OUT)/crt0.o $(OUT)/uart.o $(OUT)/exit.o
