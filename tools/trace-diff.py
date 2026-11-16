#!/usr/bin/env python3
"""Print the first instruction where a CVA6 commit trace and a Spike commit log disagree."""

import argparse
import re
import subprocess
import sys

# CSRs whose read value differs between models by design (ids, counters, misa):
# reads of them are compared without their result
NONDET_CSRS = {
    0x301,                  # misa
    0xF11, 0xF12, 0xF13,    # mvendorid, marchid, mimpid
    0xF15,                  # mconfigptr
    0xC00, 0xC01, 0xC02,    # cycle, time, instret
    0xB00, 0xB02,           # mcycle, minstret
}
NONDET_CSRS |= set(range(0xC03, 0xC20)) | set(range(0xB03, 0xB20))

# Console routines, left out of both traces: Spike polls tohost, CVA6 does not
SKIP_FUNCS = ("platform_putchar", "exit")

# RVFI reports an ecall as retired, Spike's log does not
ECALL = 0x00000073


def symbols(elf, nm):
    """Map each symbol of the ELF to its (address, size)."""
    output = subprocess.run([nm, "-S", elf], capture_output=True, text=True, check=True).stdout
    table = {}
    for line in output.splitlines():
        fields = line.split()
        if len(fields) == 4:
            table[fields[3]] = (int(fields[0], 16), int(fields[1], 16))
    return table


def entry_point(elf, readelf):
    output = subprocess.run([readelf, "-h", elf], capture_output=True, text=True, check=True).stdout
    return int(re.search(r"Entry point address:\s+0x([0-9a-f]+)", output).group(1), 16)


def is_nondet_csr_read(insn):
    return insn & 0x7F == 0x73 and (insn >> 12) & 7 != 0 and (insn >> 20) in NONDET_CSRS


class Record:
    """One retired instruction: privilege, pc, encoding, integer register write
    and store. Loads are compared through the register they write."""

    __slots__ = ("line", "priv", "pc", "insn", "rd", "rd_val", "st_addr", "st_val")

    def key(self):
        rd_val = None if is_nondet_csr_read(self.insn) else self.rd_val
        return (self.priv, self.pc, self.insn, self.rd, rd_val, self.st_addr, self.st_val)


def store_size(insn):
    """Bytes written by a store or AMO, or None."""
    opcode = insn & 0x7F
    funct3 = (insn >> 12) & 7
    if opcode in (0x23, 0x2B):           # STORE, custom-1 typed store
        return 1 << funct3
    if opcode == 0x5B and funct3 == 0:   # domcall: its 8-byte DRS record
        return 8
    if opcode == 0x2F:                   # AMO; LR writes nothing (RVFI reports it as a store)
        if insn >> 27 == 0b00010:
            return None
        return 4 if funct3 == 2 else 8
    if insn & 3 != 3:                    # compressed sw/sd/swsp/sdsp
        quadrant, funct = insn & 3, (insn >> 13) & 7
        if (quadrant, funct) in ((0, 6), (2, 6)):
            return 4
        if (quadrant, funct) in ((0, 7), (2, 7)):
            return 8
    return None


# Spike: core   0: 3 0x0000000080000000 (0x00c0006f) x5  0x... mem 0x... 0x...
SPIKE_RE = re.compile(r"core\s+\d+:\s+(\d)\s+0x([0-9a-f]+)\s+\(0x([0-9a-f]+)\)(.*)")
# CVA6: 3 0x0000000080000000 (0x00c0006f) x 5 0x... mem 0x... 0x...
CVA6_RE = re.compile(r"([0-3])\s+0x([0-9a-f]+)\s+\(0x([0-9a-f]+)\)(.*)")


def parse(path, regex, xlen_mask):
    records = []
    with open(path, errors="replace") as trace:
        for line_number, line in enumerate(trace, 1):
            match = regex.match(line)
            if not match:
                continue
            record = Record()
            record.line = line_number
            record.priv = int(match.group(1))
            record.pc = int(match.group(2), 16) & xlen_mask
            record.insn = int(match.group(3), 16)
            record.rd = record.rd_val = record.st_addr = record.st_val = None
            tokens = match.group(4).replace("x ", "x").split()
            index = 0
            while index < len(tokens):
                token = tokens[index]
                if re.fullmatch(r"x\d+", token):
                    if int(token[1:]) != 0:
                        record.rd, record.rd_val = int(token[1:]), int(tokens[index + 1], 16)
                    index += 2
                elif token == "mem":
                    size = store_size(record.insn)
                    # a store logs "mem <addr> <data>", a load "mem <addr>"
                    if size is not None and index + 2 < len(tokens) and tokens[index + 2].startswith("0x"):
                        record.st_addr = int(tokens[index + 1], 16)
                        record.st_val = int(tokens[index + 2], 16) & ((1 << (8 * size)) - 1)
                        index += 3
                    else:
                        index += 2
                else:
                    index += 1   # CSR writes (Spike only), fp registers, ...
            records.append(record)
    return records


def in_ranges(pc, ranges):
    return any(start <= pc < end for start, end in ranges)


def filtered(records, entry, skip):
    """Drop what precedes the ELF entry point (boot ROMs differ) and the skipped functions."""
    kept = []
    started = False
    for record in records:
        if not started:
            if record.pc != entry:
                continue
            started = True
        if in_ranges(record.pc, skip):
            continue
        kept.append(record)
    return kept


def describe(record):
    text = f"priv={record.priv} pc=0x{record.pc:016x} insn=0x{record.insn:08x}"
    if record.rd is not None:
        text += f" x{record.rd}=0x{record.rd_val:016x}"
    if record.st_addr is not None:
        text += f" store [0x{record.st_addr:016x}]=0x{record.st_val:x}"
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf")
    parser.add_argument("cva6_trace", help="trace_rvfi_hart_00.dasm")
    parser.add_argument("spike_log", help="output of spike --log-commits")
    parser.add_argument("--cross", default="riscv64-unknown-elf-")
    parser.add_argument("--context", type=int, default=8)
    args = parser.parse_args()

    table = symbols(args.elf, args.cross + "nm")
    skip = [(table[name][0], table[name][0] + table[name][1]) for name in SKIP_FUNCS if name in table]
    entry = entry_point(args.elf, args.cross + "readelf")
    xlen_mask = (1 << 64) - 1

    cva6 = [record for record in filtered(parse(args.cva6_trace, CVA6_RE, xlen_mask), entry, skip)
            if record.insn != ECALL]
    spike = filtered(parse(args.spike_log, SPIKE_RE, xlen_mask), entry, skip)

    # Exit with 1 at the first difference, with the instructions before it
    for position, (cva6_record, spike_record) in enumerate(zip(cva6, spike)):
        if cva6_record.key() != spike_record.key():
            print(f"first mismatch at retired instruction {position}:")
            for previous in range(max(0, position - args.context), position):
                print(f"   same  {describe(spike[previous])}")
            print(f"  spike  {describe(spike_record)}   (log line {spike_record.line})")
            print(f"  cva6   {describe(cva6_record)}   (trace line {cva6_record.line})")
            return 1

    # Exit with 0 when the traces match up to the end of the shorter one
    matched = min(len(cva6), len(spike))
    if len(cva6) != len(spike):
        longer, name = (cva6, "cva6") if len(cva6) > len(spike) else (spike, "spike")
        # The models stop at different points of the exit loop; only report
        # extra instructions outside of it.
        extra = [record for record in longer[matched:] if not in_ranges(record.pc, skip)]
        if extra:
            print(f"{name} retires {len(extra)} more instructions after {matched} matching ones; next:")
            print(f"  {name}  {describe(extra[0])}")
            return 1
    print(f"traces match: {matched} instructions")
    return 0


if __name__ == "__main__":
    sys.exit(main())
