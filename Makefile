CROSS ?= riscv64-unknown-elf-
CC := $(CROSS)gcc

# KEYSRC: where keys come from, pmp (PMP entries) or sv39 (Sv39 PTEs)
# PLATFORM: link address and console, env/platform/<name>/platform.mk
# QUICK=1: sampled t-acc rights, for Verilator
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

# The simulators have no default path. Set SPIKE (a spike built from the
# jitdomain branch of riscv-isa-sim), QEMU (a qemu-system-riscv64 built from the
# jitdomain branch of the QEMU fork) and CVA6_REPO_DIR in the environment or on
# the command line; each is checked only when a recipe runs that simulator.
ISA ?= rv64imac_zicsr_zifencei_zicntr_zicfilp_zicbom_zicboz_smrnmi_xjitdomain
SPIKE_FLAGS := --isa=$(ISA) --pmpregions=8

# Svade, as on Spike and CVA6: a clear A or D bit raises a page fault.
# Smrnmi with Smdbltrp, so that t-rnmi can raise an RNMI through a double trap;
# the vectors are fixed in env/crt.S.
RNMI_VEC := $(shell printf '0x%x' $$(($(LOAD_ADDR) + 4)))
RNMI_EXC_VEC := $(shell printf '0x%x' $$(($(LOAD_ADDR) + 8)))
QEMU_CPU ?= rv64,num-pmp-regions=8,zicfilp=true,svade=true,svadu=false,xjitdomain=true,smrnmi=true,smdbltrp=true,rnmi-interrupt-vector=$(RNMI_VEC),rnmi-exception-vector=$(RNMI_EXC_VEC)
QEMU_FLAGS := -M spike -nographic -bios none -cpu $(QEMU_CPU) -kernel

# The CVA6 Verilator testbench, built with `make verilate` in the cva6 repo
# given by CVA6_REPO_DIR (the variable the cva6 Makefile reads too).
# Each run leaves its commit trace in $(BUILD)/<test>.cva6/.
CVA6_SIM ?= $(CVA6_REPO_DIR)/work-ver/Variane_testharness
CVA6_CYCLES ?= 50000000
# Spike configured as the CVA6 core, for trace-<test>
CVA6_ISA ?= rv64imafdc_zicsr_zifencei_zicntr_xjitdomain

# SIM: simulator for run and run-<test>, spike, qemu or cva6
SIM ?= spike
RUN_spike := $(SPIKE) $(SPIKE_FLAGS)
RUN_qemu := $(QEMU) $(QEMU_FLAGS)
RUN_cva6 := tools/run-cva6.sh $(CVA6_SIM) $(CVA6_CYCLES)
RUN := $(RUN_$(SIM))
ifeq ($(RUN),)
$(error SIM=$(SIM) is not supported)
endif
NEED_spike := SPIKE
NEED_qemu := QEMU
NEED_cva6 := CVA6_REPO_DIR
need = $(if $($(1)),,$(error $(1) is not set, see the README))

TESTS := t-csr t-typed t-acc t-lpad t-drs t-dsp t-trap t-flush t-deputy t-rnmi t-spec
# Tests that only apply to one key source
TESTS_sv39 := t-pte
TESTS += $(TESTS_$(KEYSRC))
BUILD := build/$(PLATFORM)-$(KEYSRC)$(if $(filter 1,$(QUICK)),-quick)

ENV_SRCS := env/crt.S env/harness.c env/stubs.S env/keysrc-$(KEYSRC).c $(PLATFORM_SRCS)
ENV_DEPS := $(ENV_SRCS) $(wildcard env/*.h) env/link.ld env/platform/$(PLATFORM)/platform.mk

all: $(TESTS:%=$(BUILD)/%.elf)

# Expand the wildcard once % is known, so that each ELF depends on its .S
.SECONDEXPANSION:
$(BUILD)/%.elf: tests/%.c $$(wildcard tests/$$*.S) $(ENV_DEPS) | $(BUILD)
	$(CC) $(CFLAGS) -T env/link.ld $(ENV_SRCS) $(wildcard tests/$*.S) $< -o $@

$(BUILD):
	mkdir -p $@

run: all
	@$(call need,$(NEED_$(SIM)))
	@fail=0; \
	for t in $(TESTS); do \
	  if $(RUN) $(BUILD)/$$t.elf; then echo "PASS $$t"; \
	  else echo "FAIL $$t"; fail=1; fi; \
	done; \
	exit $$fail

# Benchmarks: bench/<name>.c with its stubs in bench/<name>.S. They print
# cycle counts, which only mean something on CVA6.
BENCHES := $(patsubst bench/%.c,%,$(wildcard bench/*.c))
# Keep the ELFs, which make would otherwise delete as intermediate files
.SECONDARY: $(BENCHES:%=$(BUILD)/%.elf)

$(BUILD)/%.elf: bench/%.c $$(wildcard bench/$$*.S) $(ENV_DEPS) | $(BUILD)
	$(CC) $(CFLAGS) -T env/link.ld $(ENV_SRCS) $(wildcard bench/$*.S) $< -o $@

bench: $(BENCHES:%=bench-%)

bench-%: $(BUILD)/%.elf
	@$(call need,$(NEED_$(SIM)))
	$(RUN) $<

run-%: $(BUILD)/%.elf
	@$(call need,$(NEED_$(SIM)))
	$(RUN) $<

# Compare the commit trace of the last SIM=cva6 run of a test with Spike's,
# and print the first difference
trace-%: $(BUILD)/%.elf
	@$(call need,SPIKE)
	timeout 600 $(SPIKE) --isa=$(CVA6_ISA) --pmpregions=8 --log-commits $< 2>&1 >/dev/null \
	  | head -n 20000000 >$(BUILD)/$*.spike.log || true
	tools/trace-diff.py $< $(BUILD)/$*.cva6/trace_rvfi_hart_00.dasm $(BUILD)/$*.spike.log

# Linux user-space tests, user/u-*.c, run under QEMU linux-user: QEMU_USER
# is a qemu-riscv64 built from the jitdomain branch of the QEMU fork
LINUX_CC ?= riscv64-linux-gnu-gcc
QEMU_USER_CPU ?= rv64,xjitdomain=true
USER_TESTS := u-enable u-pkey u-matrix u-access u-drs u-domain u-sigsys u-fault u-signal
USER_BUILD := build/user
USER_CFLAGS := -O2 -g -Wall -Wextra -std=gnu11 -static -pthread
RUN_USER = $(QEMU_USER) -cpu $(QEMU_USER_CPU)

user: $(USER_TESTS:%=$(USER_BUILD)/%.elf)

$(USER_BUILD)/%.elf: user/%.c user/user.c user/user.h env/jitdomain.h | $(USER_BUILD)
	$(LINUX_CC) $(USER_CFLAGS) user/user.c $< -o $@

$(USER_BUILD):
	mkdir -p $@

run-user: user
	@$(call need,QEMU_USER)
	@fail=0; \
	for t in $(USER_TESTS); do \
	  if $(RUN_USER) $(USER_BUILD)/$$t.elf; then echo "PASS $$t"; \
	  else echo "FAIL $$t"; fail=1; fi; \
	done; \
	exit $$fail

run-u-%: $(USER_BUILD)/u-%.elf
	@$(call need,QEMU_USER)
	$(RUN_USER) $<

clean:
	rm -rf build

.PHONY: all run bench user run-user clean
