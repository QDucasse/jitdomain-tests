CROSS ?= riscv64-unknown-elf-
CC := $(CROSS)gcc

# No C extension: the harness skips a faulting M-mode instruction with mepc += 4
ARCH := -march=rv64ima_zicsr_zifencei -mabi=lp64
CFLAGS := $(ARCH) -mcmodel=medany -O2 -g -Wall -Wextra -std=gnu11 \
          -ffreestanding -fno-builtin -nostdlib -nostartfiles -Ienv \
          -Wl,--no-warn-rwx-segments

SPIKE ?= $(abspath ../riscv-isa-sim/install/bin/spike)
ISA ?= rv64ima_zicsr_zifencei_xjitdomain

TESTS :=
BUILD := build

ENV_SRCS := env/crt.S env/harness.c env/stubs.S
ENV_DEPS := $(ENV_SRCS) $(wildcard env/*.h) env/link.ld

all: $(TESTS:%=$(BUILD)/%.elf)

$(BUILD)/%.elf: tests/%.c $(wildcard tests/%.S) $(ENV_DEPS) | $(BUILD)
	$(CC) $(CFLAGS) -T env/link.ld $(ENV_SRCS) $(wildcard tests/$*.S) $< -o $@

$(BUILD):
	mkdir -p $@

run: all
	@fail=0; \
	for t in $(TESTS); do \
	  if $(SPIKE) --isa=$(ISA) $(BUILD)/$$t.elf; then echo "PASS $$t"; \
	  else echo "FAIL $$t"; fail=1; fi; \
	done; \
	exit $$fail

run-%: $(BUILD)/%.elf
	$(SPIKE) --isa=$(ISA) $<

clean:
	rm -rf $(BUILD)

.PHONY: all run clean
