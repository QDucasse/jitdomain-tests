# JITDomain conformance suite

Directed, self-checking bare-metal tests for JITDomain, shared by Spike, QEMU and the CVA6 RTL.

> *Note:* The v1 suite (patched binutils, CHDOM/RETDOM, Verilator runner) is kept at tag `v1`.

## Running

```
make run                      # build everything and run it on Spike
make run-t-acc                # one test
make run SPIKE=/path/to/spike ISA=rv64imac_zicsr_zifencei_zicfilp_xjitdomain
make all QUICK=1              # sampled T-ACC, for Verilator
make run KEYSRC=sv39          # keys from the page tables
```

| Variable | Default | Meaning |
|---|---|---|
| `KEYSRC` | `pmp` | where keys come from: `pmp` (PMP entries, through `dmpcfg`) or `sv39` (Sv39 PTEs) |
| `PLATFORM` | `htif` | link address and console, from `env/platform/<name>/platform.mk` |
| `QUICK` | `0` | `1`: T-ACC tries three rights values per cell (none, exactly the needed right, all but it) instead of eight |

ELFs go to `build/<platform>-<keysrc>[-quick]/`. The `htif` ELFs load at `0x80000000` and use HTIF `tohost` for the
console and the exit code, so the same file runs on Spike, QEMU (`spike` machine) and the CVA6 Verilator testbench.

Requirements: a stock `riscv64-unknown-elf-gcc` and a Spike built from the `jitdomain` branch of riscv-isa-sim (by
default `../riscv-isa-sim/install/bin/spike`). Spike runs with `--pmpregions=8`, CVA6's PMP count, and with Zicfilp;
`t-lpad` detects Zicfilp at run time and skips its ELP checks without it, as on CVA6. Each test prints
`<name>: N checks, M failures` and exits with a non-zero code on failure.

## Layout

| Path | Content |
|---|---|
| `env/jitdomain.h` | CSR numbers, field layouts, causes; in assembly, `.insn` macros for `dl*`/`ds*`/`domcall`/`domret`/`domlpad` |
| `env/crt.S` | entry, trap vector, `run_lower()` |
| `env/harness.c` | trap handler, `printf`, `CHECK_*`, domain configuration helpers |
| `env/keysrc-pmp.c` | `keysrc_reset()` and `key_set(page, key)` through PMP and `dmpcfg` |
| `env/keysrc-sv39.c` | the same through the PTE key bits of an identity map |
| `env/sv39.h` | PTE bits, `pte()`, `pte_key()` and `sv39_root()`, shared with `t-pte` |
| `env/platform/htif/` | `platform.mk` (`LOAD_ADDR`, sources) and the HTIF console and exit |
| `env/stubs.S` | the S/U-mode stubs the tests run |
| `tests/t-*.c`, `tests/t-*.S` | one ELF per test ID, with its own stubs if it needs any; `t-pte` is built only with `KEYSRC=sv39` |

The toolchain needs no JITDomain support: the instructions are written with `.insn` (see `env/jitdomain.h`). A new
platform (the ZCU104, for instance) adds `env/platform/<name>/` with its `LOAD_ADDR`, `platform_putchar()` and
`exit()`; the tests do not change.

## How a test runs

Each test is an M-mode monitor. It configures the domain state, then runs a stub in S or U mode with `run_lower(entry,
priv, mdstatus, a0, a1)`, which enters the stub through `mret`. That way the domain (`MPD`) and a pending entry
(`MPPEND`) come from the trap-return path. The stub does one thing and then executes `ecall`; the first trap ends it,
and `run_lower` returns `mcause`, with the rest of the trap, all registers included, in `last_trap`. A trap taken in M
mode skips the faulting instruction by its length, 2 or 4 bytes. The suite is built with C.

Tests tag memory only through `dom_reset()`, `dom_disable()` and `key_set()`, so they run unchanged with either key
source. With `KEYSRC=pmp`, `dom_reset()` builds a TOR chain over the 8 PMP entries:

| PMP entry | Region | Key |
|---|---|---|
| 0 | `[0, pages[1])`: code, data and page 0 | 0 |
| 1–6 | `pages[k]`, 4 KiB each | `k` |
| 7 | `pages[7]` and the two DRS pages | 7 |

All entries are RWX, so only the domain check can deny an access. The stacks, above the DRS, match no entry and are
out of reach of S and U mode. `dom_reset()` sets `dstatus = EN | DRSKEY=7` and leaves the other CSRs at their reset
values.

With `KEYSRC=sv39`, `dom_reset()` builds an identity map of the image, the pages and the DRS with 4 KiB leaves, all
RWX, A and D set: the image has key 0, `pages[k]` key `k`, the DRS key 7, and the stacks are not mapped. S mode cannot
fetch from U pages, so there are two copies of the tables, one with U leaves and one with S leaves; `run_lower()` sets
`satp` for the stub's privilege. `key_set(k, key)` retags only `pages[k]`. With `dstatus.EN = 0` the key bits are
reserved again, so `dom_disable()` clears every key first. One PMP entry covers the map, and its key is ignored.

M mode never translates, so the locked-PMP checks in `t-acc` run only with `KEYSRC=pmp`.

## Status

| ID | Test | State |
|---|---|---|
| T-ACC | `t-acc` | done: every `(e, k, rights)` for load, store, AMO, LR and fetch; SCHK; locked PMP in M mode |
| T-TYPED | `t-typed` | done; also covers T-ORD |
| T-ORD | `t-typed` | done |
| T-CSR | `t-csr` | done |
| T-PTE | `t-pte` | done: key per mapping, non-leaf keys, keys with `EN = 0`, PTE checks first, `dmpcfg` ignored, superpages |
| T-LPAD | `t-lpad` | done: pad matching, alignment, pad fetch rights, gate, own domain, pads without `domcall`, plain jumps, Zicfilp ELP |
| T-DRS | `t-drs` | done: round trip, `domret` target and domain, nesting 64 deep, bad `dsp` and records, `DRSKEY`, U/S access to DRS pages |
| T-TRAP | | todo |
| T-SIG | | Linux user mode, todo |
| T-SPEC | | RTL only |
| T-DEPUTY | | todo |

Not covered yet: `dmpcfg1` (illegal with 8 PMP entries), `cbo.*`.
