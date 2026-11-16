#!/bin/sh
# Run one ELF on the CVA6 Verilator testbench, in a directory of its own next
# to the ELF so that the commit trace (trace_rvfi_hart_00.dasm) is kept.
# The debug module is disabled: the testbench preloads the ELF and takes the
# console and the exit code from the stores to tohost in the commit trace.
# Usage: run-cva6.sh <Variane_testharness> <cycles> <elf> [nm]
set -e
sim=$(realpath "$1") cycles=$2 elf=$(realpath "$3") nm=${4:-riscv64-unknown-elf-nm}
tohost=$($nm "$elf" | awk '$3 == "tohost" { print $1 }')
dir=${elf%.elf}.cva6
mkdir -p "$dir"
cd "$dir"
status=0
"$sim" "$elf" +elf_file="$elf" +tohost_addr="$tohost" +debug_disable=1 +time_out="$cycles" \
  >sim.log 2>&1 || status=$?
cat sim.log
# The testbench reports success when it is killed; only a test that wrote its
# exit code to tohost has finished
grep -q 'Simulation terminated after' sim.log || exit 1
exit $status
