# JITDomain conformance suite

Directed, self-checking bare-metal tests for JITDomain, shared by Spike, QEMU and the CVA6 RTL.

> *Note:* The v1 suite (patched binutils, CHDOM/RETDOM, Verilator runner) is kept at tag `v1`.

## Running

```
make run                      # build everything and run it on Spike
make run-t-acc                # one test
make run SPIKE=/path/to/spike ISA=rv64imac_zicsr_zifencei_xjitdomain
```

| Variable | Default | Meaning |
|---|---|---|
| `KEYSRC` | `pmp` | where keys come from: `pmp` (Track A); `sv39` (Track B) is not written yet |
| `PLATFORM` | `htif` | link address and console, from `env/platform/<name>/platform.mk` |

ELFs go to `build/<platform>-<keysrc>/`. The `htif` ELFs load at `0x80000000` and use HTIF `tohost` for the
console and the exit code, so the same file runs on Spike, QEMU (`spike` machine) and the CVA6 Verilator testbench.

Requirements: a stock `riscv64-unknown-elf-gcc` and a Spike built from the `jitdomain` branch of riscv-isa-sim (by
default `../riscv-isa-sim/install/bin/spike`). Spike runs with `--pmpregions=8`, CVA6's PMP count. Each test prints
`<name>: N checks, M failures` and exits with a non-zero code on failure.

## Layout

| Path | Content |
|---|---|
| `env/jitdomain.h` | CSR numbers, field layouts, causes; in assembly, `.insn` macros for `dl*`/`ds*`/`domcall`/`domret`/`domlpad` |
| `env/crt.S` | entry, trap vector, `run_lower()` |
| `env/harness.c` | trap handler, `printf`, `CHECK_*`, domain configuration helpers |
| `env/keysrc-pmp.c` | Track A: `keysrc_reset()` and `key_set(page, key)` through PMP and `dmpcfg` |
| `env/platform/htif/` | `platform.mk` (`LOAD_ADDR`, sources) and the HTIF console and exit |
| `env/stubs.S` | the S/U-mode stubs the tests run |
| `tests/t-*.c` | one ELF per test ID |

The toolchain needs no JITDomain support: the instructions are written with `.insn` (see `env/jitdomain.h`). A new
platform (the ZCU104, for instance) adds `env/platform/<name>/` with its `LOAD_ADDR`, `platform_putchar()` and
`exit()`; the tests do not change.

## How a test runs

Each test is an M-mode monitor. It configures the domain state, then runs a stub in S or U mode with
`run_lower(entry, priv, mdstatus, a0, a1)`, which enters the stub through `mret`. That way the domain (`MPD`) and a
pending entry (`MPPEND`) come from the trap-return path. The stub does one thing and then executes `ecall`;
the first trap ends it, and `run_lower` returns `mcause`, with the rest of the trap in `last_trap`. A trap taken in
M mode skips the faulting instruction by its length, 2 or 4 bytes. The suite is built with C.

Tests tag memory only through `dom_reset()` and `key_set()`, so they run unchanged with either key source. On Track A,
`dom_reset()` builds a TOR chain over the 8 PMP entries:

| PMP entry | Region | Key |
|---|---|---|
| 0 | `[0, pages[1])`: code, data and page 0 | 0 |
| 1–6 | `pages[k]`, 4 KiB each | `k` |
| 7 | `pages[7]` and the two DRS pages | 7 |

All entries are RWX, so only the domain check can deny an access. The stacks, above the DRS, match no entry and are
out of reach of S and U mode. `dom_reset()` sets `dstatus = EN | DRSKEY=7` and leaves the other CSRs at their reset
values.

## Status

| ID | Test | State |
|---|---|---|
| T-ACC | `t-acc` | done: every `(e, k, rights)` for load, store, AMO, LR and fetch; SCHK; locked PMP in M mode |
| T-TYPED | `t-typed` | done; also covers T-ORD |
| T-ORD | `t-typed` | done |
| T-CSR | `t-csr` | done |
| T-LPAD | | todo |
| T-DRS | | todo |
| T-TRAP | | todo |
| T-SIG | | Linux user mode, todo |
| T-SPEC | | RTL only |
| T-DEPUTY | | todo |

Not covered yet: Track B (PTE keys under Sv39), `dmpcfg1` (illegal with 8 PMP entries), `cbo.*`.
