TEST_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
PROJECT_ROOT := $(abspath $(TEST_DIR)/../../../..)
include $(PROJECT_ROOT)/tests/support/platform.mk
OUTPUT_DIR ?= $(error OUTPUT_DIR required)
CONFIG ?= $(error CONFIG required)
TILES ?= $(error TILES required)
CXX := $(GOLEM_LLVM_DIR)/bin/clang++
FLAGS := --target=riscv64-unknown-elf -mcpu=golem-analog -march=rv64imafdc_zve32f_zvl256b -mabi=lp64d -mcmodel=medany -ffreestanding -fno-stack-protector -O2 -g
CXXFLAGS := $(FLAGS) -std=c++20 -fno-exceptions -fno-rtti -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-vectorize -fno-slp-vectorize -Wall -Wextra -Werror -I$(PLATFORM_ROOT) -I$(dir $(CONFIG))
COMMON := $(OUTPUT_DIR)/crt0.o $(OUTPUT_DIR)/uart.o $(OUTPUT_DIR)/exit.o $(OUTPUT_DIR)/memory.o
.PHONY: all
all: $(TILES:%=$(OUTPUT_DIR)/tile%.elf)
$(OUTPUT_DIR):
	mkdir -p -- $@
$(OUTPUT_DIR)/crt0.o: $(PLATFORM_STARTUP_ROOT)/crt0.S | $(OUTPUT_DIR)
	$(CXX) $(FLAGS) -c $< -o $@
$(OUTPUT_DIR)/uart.o: $(PLATFORM_ROOT)/uart.cpp | $(OUTPUT_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(OUTPUT_DIR)/exit.o: $(PLATFORM_STARTUP_ROOT)/platform-exit.cpp | $(OUTPUT_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(OUTPUT_DIR)/memory.o: $(PROJECT_ROOT)/src/platform/runtime/freestanding-memory.cpp | $(OUTPUT_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(OUTPUT_DIR)/tile%.o: $(TEST_DIR)/main.cpp $(CONFIG) | $(OUTPUT_DIR)
	$(CXX) $(CXXFLAGS) -DTILE_ID=$* -c $< -o $@
$(OUTPUT_DIR)/tile%.elf: $(OUTPUT_DIR)/tile%.o $(COMMON) $(PLATFORM_STARTUP_ROOT)/tile.ld
	$(CXX) $(FLAGS) -nostdlib -nostartfiles -nodefaultlibs -fuse-ld=lld -Wl,--build-id=none -Wl,-T,$(PLATFORM_STARTUP_ROOT)/tile.ld $(COMMON) $< -o $@
