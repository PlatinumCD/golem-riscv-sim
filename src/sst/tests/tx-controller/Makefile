TEST_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
PROJECT_ROOT := $(abspath $(TEST_DIR)/../../../..)
include $(PROJECT_ROOT)/tests/support/platform.mk
LLVM ?= $(if $(GOLEM_LLVM_DIR),$(GOLEM_LLVM_DIR),$(INSTALL_ROOT)/llvm)
TARGET ?= riscv64-unknown-elf
CPU ?= golem-analog
ABI ?= lp64d
OUTPUT_DIR ?= $(BUILD_ROOT)/tests/tx-controller/elf
CLANGXX ?= $(LLVM)/bin/clang++
PATTERN ?= 0
BANK_LAYOUT ?= 0
D ?= 4096
SKEW ?= 0
RECORD_READY ?= 0
TILES := 1 2 3 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 21 23
ELFS := $(addprefix $(OUTPUT_DIR)/tile,$(addsuffix .elf,$(TILES)))
COMMON_FLAGS := --target=$(TARGET) -mcpu=$(CPU) -mabi=$(ABI) -mcmodel=medany -ffreestanding -fno-stack-protector -fno-function-sections -fdata-sections -O2 -g
CXX_FLAGS := $(COMMON_FLAGS) -std=c++20 -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-vectorize -fno-slp-vectorize -Wall -Wextra -Wpedantic -I$(PLATFORM_ROOT)
COMMON_OBJECTS := $(OUTPUT_DIR)/crt0.o $(OUTPUT_DIR)/uart.o $(OUTPUT_DIR)/platform-exit.o
.PHONY: all
all: $(ELFS)
$(OUTPUT_DIR):
	mkdir -p -- $@
$(OUTPUT_DIR)/crt0.o: $(PLATFORM_STARTUP_ROOT)/crt0.S | $(OUTPUT_DIR)
	$(CLANGXX) $(COMMON_FLAGS) -c $< -o $@
$(OUTPUT_DIR)/uart.o: $(PLATFORM_ROOT)/uart.cpp | $(OUTPUT_DIR)
	$(CLANGXX) $(CXX_FLAGS) -c $< -o $@
$(OUTPUT_DIR)/platform-exit.o: $(PLATFORM_STARTUP_ROOT)/platform-exit.cpp | $(OUTPUT_DIR)
	$(CLANGXX) $(CXX_FLAGS) -c $< -o $@
$(OUTPUT_DIR)/tile%.o: $(TEST_DIR)/main.cpp $(TEST_DIR)/Makefile | $(OUTPUT_DIR)
	$(CLANGXX) $(CXX_FLAGS) -DMITTENS_TILE_ID=$* \
		-DMITTENS_FANOUT_PATTERN=$(PATTERN) \
		-DMITTENS_FANOUT_BANK_LAYOUT=$(BANK_LAYOUT) \
		-DMITTENS_FANOUT_PAYLOAD_BYTES=$(D) \
		-DMITTENS_FANOUT_READY_SKEW_CYCLES=$(SKEW) \
		-DMITTENS_FANOUT_RECORD_READY_CYCLES=$(RECORD_READY) -c $< -o $@
$(OUTPUT_DIR)/tile%.elf: $(OUTPUT_DIR)/tile%.o $(COMMON_OBJECTS) $(PLATFORM_STARTUP_ROOT)/scratchpad.ld
	$(CLANGXX) $(COMMON_FLAGS) -nostdlib -nostartfiles -nodefaultlibs -fuse-ld=lld -Wl,--build-id=none -Wl,--gc-sections -Wl,--defsym,SPM_BYTES=32768,--defsym,SPM_CODE_OFFSET=24576,--defsym,STACK_BYTES=2048 -Wl,-T,$(PLATFORM_STARTUP_ROOT)/scratchpad.ld $(COMMON_OBJECTS) $< -o $@
