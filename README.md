# JITDomain conformance suite

Directed, self-checking bare-metal tests for JITDomain, shared by Spike, QEMU and the CVA6 RTL.

> *Note:* The v1 suite (patched binutils, CHDOM/RETDOM, Verilator runner) is kept at tag `v1`.

## Running

```
make run                      # build everything and run it on Spike
make run-t-acc                # one test
make run SPIKE=/path/to/spike ISA=rv64imac_zicsr_zifencei_xjitdomain
```

Requirements: a stock `riscv64-unknown-elf-gcc` and a Spike built from the `jitdomain` branch of riscv-isa-sim (by
default `../riscv-isa-sim/install/bin/spike`). Each test prints `<name>: N checks, M failures` and exits through HTIF
`tohost` with a non-zero code on failure.

## Layout

| Path | Content |
|---|---|
| `env/jitdomain.h` | CSR numbers, field layouts, causes; in assembly, `.insn` macros for `dl*`/`ds*`/`domcall`/`domret`/`domlpad` |
| `env/crt.S` | entry, trap vector, `run_lower()` |
| `env/harness.c` | trap handler, HTIF console, `CHECK_*`, domain configuration helpers |
| `env/stubs.S` | the S/U-mode stubs the tests run |
| `tests/t-*.c` | one ELF per test ID |

The toolchain needs no JITDomain support: the instructions are written with `.insn` (see `env/jitdomain.h`).

## How a test runs

Each test is an M-mode monitor. It configures the domain state, then runs a stub in S or U mode with
`run_lower(entry, priv, mdstatus, a0, a1)`, which enters the stub through `mret`. That way the domain (`MPD`) and a
pending entry (`MPPEND`) come from the trap-return path. The stub does one thing and then executes `ecall`;
the first trap ends it, and `run_lower` returns `mcause`, with the rest of the trap in `last_trap`. A trap taken in
M mode skips the faulting instruction, so the suite is built without the C extension.

Memory is tagged through PMP (Track A):

| PMP entry | Region | Key |
|---|---|---|
| 0–7 | `pages[k]`, 4 KiB each | `k` |
| 8 | `drs_base`, 8 KiB | 7 |
| 15 | everything else | 0 |

All entries are RWX, so only the domain check can deny an access. `dom_reset()` sets `dstatus = EN | DRSKEY=7` and
leaves the other CSRs at their reset values.

## Status

| ID | Test | State |
|---|---|---|
| T-ACC | | todo |
| T-TYPED | | todo |
| T-ORD | | todo |
| T-CSR | | todo |
| T-LPAD | | todo |
| T-DRS | | todo |
| T-TRAP | | todo |
| T-SIG | | Linux user mode, todo |
| T-SPEC | | RTL only |
| T-DEPUTY | | todo |

Not covered yet: Track B (PTE keys under Sv39), `dmpcfg1`, `cbo.*`.
