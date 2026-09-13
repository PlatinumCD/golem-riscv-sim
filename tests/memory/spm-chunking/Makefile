TEST_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
PROJECT_ROOT := $(abspath $(TEST_DIR)/../../..)
include $(PROJECT_ROOT)/tests/support/platform.mk
OUT ?= $(error OUT required)
DATA_BYTES ?= 65548
CHUNK_BYTES ?= 4096
CXX := $(GOLEM_LLVM_DIR)/bin/clang++
FLAGS := --target=riscv64-unknown-elf -mcpu=golem-analog -mabi=lp64d -mcmodel=medany -ffreestanding -fno-stack-protector -O2 -g
CXXFLAGS := $(FLAGS) -std=c++20 -fno-exceptions -fno-rtti -fno-unwind-tables -fno-asynchronous-unwind-tables -I$(PLATFORM_ROOT)
OBJECTS := $(OUT)/crt0.o $(OUT)/uart.o $(OUT)/exit.o $(OUT)/main.o
.PHONY: all
all: $(OUT)/tile.elf
$(OUT):
	mkdir -p -- $@
$(OUT)/crt0.o: $(PLATFORM_STARTUP_ROOT)/crt0.S | $(OUT)
	$(CXX) $(FLAGS) -c $< -o $@
$(OUT)/uart.o: $(PLATFORM_ROOT)/uart.cpp | $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(OUT)/exit.o: $(PLATFORM_STARTUP_ROOT)/platform-exit.cpp | $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(OUT)/main.o: $(TEST_DIR)/main.cpp | $(OUT)
	$(CXX) $(CXXFLAGS) -DDATA_BYTES=$(DATA_BYTES) -DCHUNK_BYTES=$(CHUNK_BYTES) -c $< -o $@
$(OUT)/tile.elf: $(OBJECTS) $(TEST_DIR)/layout.ld
	$(CXX) $(FLAGS) -nostdlib -fuse-ld=lld -Wl,-T,$(TEST_DIR)/layout.ld -Wl,-Map,$@.map $(OBJECTS) -o $@
