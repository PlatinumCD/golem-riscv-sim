TEST_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
PROJECT_ROOT := $(abspath $(TEST_DIR)/../../..)
include $(PROJECT_ROOT)/tests/support/platform.mk
OUTPUT_DIR ?= $(BUILD_ROOT)/tests/platform/rvv-memory-accounting/elf
CXX := $(GOLEM_LLVM_DIR)/bin/clang++
FLAGS := --target=riscv64-unknown-elf -mcpu=golem-analog -march=rv64imafdc_zve32f_zvl256b -mabi=lp64d -mcmodel=medany -ffreestanding -fno-stack-protector -O2 -g
CXXFLAGS := $(FLAGS) -std=c++20 -fno-exceptions -fno-rtti -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-vectorize -fno-slp-vectorize -fno-builtin -Wall -Wextra -Werror -I$(PLATFORM_ROOT)
COMMON := $(OUTPUT_DIR)/crt0.o $(OUTPUT_DIR)/uart.o $(OUTPUT_DIR)/exit.o
.PHONY: all
all: $(addprefix $(OUTPUT_DIR)/kernel,$(addsuffix .elf,0 1 2))
$(OUTPUT_DIR):
	mkdir -p -- $@
$(OUTPUT_DIR)/crt0.o: $(PLATFORM_STARTUP_ROOT)/crt0.S | $(OUTPUT_DIR)
	$(CXX) $(FLAGS) -c $< -o $@
$(OUTPUT_DIR)/uart.o: $(PLATFORM_ROOT)/uart.cpp | $(OUTPUT_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(OUTPUT_DIR)/exit.o: $(PLATFORM_STARTUP_ROOT)/platform-exit.cpp | $(OUTPUT_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(OUTPUT_DIR)/kernel%.o: $(TEST_DIR)/main.cpp | $(OUTPUT_DIR)
	$(CXX) $(CXXFLAGS) -DKERNEL=$* -c $< -o $@
$(OUTPUT_DIR)/kernel%.elf: $(OUTPUT_DIR)/kernel%.o $(COMMON) $(PLATFORM_STARTUP_ROOT)/scratchpad.ld
	$(CXX) $(FLAGS) -nostdlib -nostartfiles -nodefaultlibs -fuse-ld=lld -Wl,--build-id=none $(TEST_SPM_LINK_FLAGS) -Wl,-T,$(PLATFORM_STARTUP_ROOT)/scratchpad.ld $(COMMON) $< -o $@
