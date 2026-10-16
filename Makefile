CROSS ?= riscv64-unknown-elf-
CC := $(CROSS)gcc

# KEYSRC: where keys come from, pmp (PMP entries) or sv39 (Sv39 PTEs)
# PLATFORM: link address and console, env/platform/<name>/platform.mk
# QUICK=1: sampled T-ACC rights, for Verilator
KEYSRC ?= pmp
PLATFORM ?= htif
QUICK ?= 0

include env/platform/$(PLATFORM)/platform.mk

ifeq ($(wildcard env/keysrc-$(KEYSRC).c),)
$(error KEYSRC=$(KEYSRC) is not implemented)
endif

ARCH := -march=rv64imac_zicsr_zifencei -mabi=lp64
CFLAGS := $(ARCH) -mcmodel=medany -O2 -g -Wall -Wextra -std=gnu11 \
          -ffreestanding -fno-builtin -nostdlib -nostartfiles -Ienv \
          -DQUICK=$(QUICK) -DKEYSRC_$(shell echo $(KEYSRC) | tr a-z A-Z)=1 \
          -Wl,--defsym=LOAD_ADDR=$(LOAD_ADDR) -Wl,--no-warn-rwx-segments

SPIKE ?= $(abspath ../riscv-isa-sim/install/bin/spike)
ISA ?= rv64imac_zicsr_zifencei_zicfilp_zicbom_zicboz_xjitdomain
SPIKE_FLAGS := --isa=$(ISA) --pmpregions=8

QEMU ?= $(abspath ../qemu/build-jitdomain/qemu-system-riscv64)
# Svade, as on Spike and CVA6: a clear A or D bit raises a page fault
QEMU_CPU ?= rv64,num-pmp-regions=8,zicfilp=true,svade=true,svadu=false,xjitdomain=true
QEMU_FLAGS := -M spike -nographic -bios none -cpu $(QEMU_CPU) -kernel

# SIM: simulator for run and run-<test>, spike or qemu
SIM ?= spike
RUN_spike := $(SPIKE) $(SPIKE_FLAGS)
RUN_qemu := $(QEMU) $(QEMU_FLAGS)
RUN := $(RUN_$(SIM))
ifeq ($(RUN),)
$(error SIM=$(SIM) is not supported)
endif

TESTS := t-csr t-typed t-acc t-lpad t-drs t-trap t-flush t-deputy
# Tests that only apply to one key source
TESTS_sv39 := t-pte
TESTS += $(TESTS_$(KEYSRC))
BUILD := build/$(PLATFORM)-$(KEYSRC)$(if $(filter 1,$(QUICK)),-quick)

ENV_SRCS := env/crt.S env/harness.c env/stubs.S env/keysrc-$(KEYSRC).c $(PLATFORM_SRCS)
ENV_DEPS := $(ENV_SRCS) $(wildcard env/*.h) env/link.ld env/platform/$(PLATFORM)/platform.mk

all: $(TESTS:%=$(BUILD)/%.elf)

$(BUILD)/%.elf: tests/%.c $(wildcard tests/%.S) $(ENV_DEPS) | $(BUILD)
	$(CC) $(CFLAGS) -T env/link.ld $(ENV_SRCS) $(wildcard tests/$*.S) $< -o $@

$(BUILD):
	mkdir -p $@

run: all
	@fail=0; \
	for t in $(TESTS); do \
	  if $(RUN) $(BUILD)/$$t.elf; then echo "PASS $$t"; \
	  else echo "FAIL $$t"; fail=1; fi; \
	done; \
	exit $$fail

run-%: $(BUILD)/%.elf
	$(RUN) $<

clean:
	rm -rf build

.PHONY: all run clean
