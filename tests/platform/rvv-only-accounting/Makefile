TEST_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
PROJECT_ROOT := $(abspath $(TEST_DIR)/../../..)

include $(PROJECT_ROOT)/tests/support/platform.mk
LLVM ?= $(if $(GOLEM_LLVM_DIR),$(GOLEM_LLVM_DIR),$(INSTALL_ROOT)/llvm)
TARGET ?= riscv64-unknown-elf
CPU ?= golem-analog
ABI ?= lp64d

CLANGXX ?= $(LLVM)/bin/clang++
OUTPUT_DIR := $(BUILD_ROOT)/tests/platform/rvv-only-accounting
RUNTIME_LIBRARY := $(INSTALL_ROOT)/runtime/lib/libgolem-runtime.a

COMMON_FLAGS := \
	--target=$(TARGET) -mcpu=$(CPU) -mabi=$(ABI) -mcmodel=medany \
	-ffreestanding -fno-stack-protector -fno-function-sections \
	-fdata-sections -O2 -g

CXX_FLAGS := $(COMMON_FLAGS) \
	-std=c++20 -fno-exceptions -fno-rtti -fno-threadsafe-statics \
	-fno-use-cxa-atexit -fno-unwind-tables \
	-fno-asynchronous-unwind-tables -Wall -Wextra -Wpedantic \
	-I$(PLATFORM_ROOT)

COMMON_OBJECTS := \
	$(OUTPUT_DIR)/crt0.o \
	$(OUTPUT_DIR)/uart.o \
	$(OUTPUT_DIR)/platform-exit.o

K_VALUES := 32 64 128 256 512 1024
ELFS := $(K_VALUES:%=$(OUTPUT_DIR)/rvv-k%.elf)

.PHONY: all clean

all: $(ELFS)

$(OUTPUT_DIR):
	mkdir -p -- $@

$(OUTPUT_DIR)/crt0.o: $(PLATFORM_STARTUP_ROOT)/crt0.S | $(OUTPUT_DIR)
	$(CLANGXX) $(COMMON_FLAGS) -c $< -o $@

$(OUTPUT_DIR)/uart.o: $(PLATFORM_ROOT)/uart.cpp \
	$(PLATFORM_ROOT)/platform.h | $(OUTPUT_DIR)
	$(CLANGXX) $(CXX_FLAGS) -c $< -o $@

$(OUTPUT_DIR)/platform-exit.o: $(PLATFORM_STARTUP_ROOT)/platform-exit.cpp \
	$(PLATFORM_ROOT)/platform.h | $(OUTPUT_DIR)
	$(CLANGXX) $(CXX_FLAGS) -c $< -o $@

$(OUTPUT_DIR)/rvv-k%.o: $(TEST_DIR)/main.cpp \
	$(PLATFORM_ROOT)/mesh-nic.h | $(OUTPUT_DIR)
	$(CLANGXX) $(CXX_FLAGS) -DRVV_COMPUTE_K=$* -c $< -o $@

define LINK_ELF
	$(CLANGXX) $(COMMON_FLAGS) -nostdlib -nostartfiles -nodefaultlibs \
		-fuse-ld=lld -Wl,--build-id=none -Wl,--gc-sections \
		-Wl,-T,$(PLATFORM_STARTUP_ROOT)/tile.ld -Wl,-Map,$@.map \
		$(COMMON_OBJECTS) $< $(RUNTIME_LIBRARY) -o $@
	@echo "built $@"
endef

$(OUTPUT_DIR)/rvv-k%.elf: $(OUTPUT_DIR)/rvv-k%.o $(COMMON_OBJECTS) \
	$(RUNTIME_LIBRARY) $(PLATFORM_STARTUP_ROOT)/tile.ld $(TEST_DIR)/Makefile
	$(LINK_ELF)

clean:
	rm -rf -- $(OUTPUT_DIR)
