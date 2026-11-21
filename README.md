# JITDomain conformance suite

Directed, self-checking tests for JITDomain, in two suites:

- **Bare-metal tests**, `tests/t-*`: M-mode programs that drive the hardware interface directly. The same ELF runs on
  Spike, QEMU and the CVA6 RTL.
- **Linux user-space tests**, `user/u-*`: Linux programs for the user-space interface (the `PR_RISCV_JITDOMAIN`
  `prctl`, the protection-key syscalls, signals). They run under QEMU linux-user only.

`bench/` holds benchmarks, which are not tests (see [Benchmarks](#benchmarks)).

Every test prints `<name>: N checks, M failures` and exits with a non-zero code on failure.

> *Note:* The v1 suite (patched binutils, CHDOM/RETDOM, Verilator runner) is kept at tag `v1`.

## Quick start

The simulators have no default path. Set them in your environment or on the command line.

```sh
make run SPIKE=/path/to/spike                                   # bare-metal tests on Spike
make run SIM=qemu QEMU=/path/to/qemu-system-riscv64             # ... on QEMU
make run SIM=cva6 QUICK=1 CVA6_REPO_DIR=/path/to/cva6           # ... on the CVA6 RTL (Verilator)
make run-user QEMU_USER=/path/to/qemu-riscv64                   # Linux user-space tests
```

## Bare-metal tests

### Requirements

| Simulator | What to build | Variable |
|---|---|---|
| (all) | a stock `riscv64-unknown-elf-gcc` | `CROSS=` (prefix) |
| Spike | Spike from the `jitdomain` branch of riscv-isa-sim | `SPIKE=` |
| QEMU | `qemu-system-riscv64` from the `jitdomain` branch of the QEMU fork | `QEMU=` |
| CVA6 | the Verilator testbench of a cva6 checkout, see [CVA6](#cva6) | `CVA6_REPO_DIR=` |

The toolchain needs no JITDomain support: the instructions are written with `.insn` (see `env/jitdomain.h`).

### Running

```sh
make run                  # build everything and run it on $(SIM)
make run-t-acc            # one test
make all                  # build only
make run KEYSRC=sv39      # keys from the page tables instead of PMP
make run ISA=rv64imac_zicsr_zifencei_zicfilp_zicbom_zicboz_smrnmi_xjitdomain   # Spike without Zicntr
```

| Variable | Default | Meaning |
|---|---|---|
| `SIM` | `spike` | simulator for `run` and `run-<test>`: `spike`, `qemu` or `cva6` |
| `KEYSRC` | `pmp` | where keys come from: `pmp` (PMP entries, through `dmpcfg`) or `sv39` (Sv39 PTEs) |
| `QUICK` | `0` | `1`: `t-acc` tries three rights values per cell (none, exactly the needed right, all but it) instead of eight; use it on Verilator |
| `PLATFORM` | `htif` | link address and console, from `env/platform/<name>/platform.mk` |
| `ISA` | see `Makefile` | Spike's `--isa` |
| `QEMU_CPU` | see `Makefile` | QEMU's `-cpu` |

ELFs go to `build/<platform>-<keysrc>[-quick]/`. The `htif` ELFs load at `0x80000000` and use HTIF `tohost` for the
console and the exit code, which is why the same file runs everywhere.

### Simulator configuration

The `Makefile` already passes these; they matter if you run an ELF by hand.

- **Spike** runs with `--pmpregions=8` (CVA6's PMP count), Zicfilp and Smrnmi.
- **QEMU** runs on the `spike` machine, with `num-pmp-regions=8`, Zicfilp, Svade (a clear A or D bit faults, as on
  Spike and CVA6), Smrnmi with Smdbltrp (so that `t-rnmi` can raise an RNMI through a double trap) and `xjitdomain`.
- **RNMI vectors** are fixed at `LOAD_ADDR + 4` and `+ 8` by `env/crt.S`, which also sets `mnstatus.NMIE` and clears
  `mstatus.MDT` at boot.

Some tests adapt to what the model supports:

- `t-lpad` detects Zicfilp at run time and skips its ELP checks without it, as on CVA6.
- `t-rnmi` skips itself without Smrnmi. Spike never takes an RNMI, so there `t-rnmi` checks only `mnret`.
- `t-spec` skips itself on models without cache timing (Spike, QEMU).
- `t-pte` is built only with `KEYSRC=sv39`.

### CVA6

`SIM=cva6` runs the suite on the CVA6 RTL, in its Verilator testbench, taken from the cva6 repo given by
`CVA6_REPO_DIR`.

Build the testbench once, in the cva6 repo. It needs Verilator 5.008, the version CVA6's CI uses (5.024 crashes on this
testbench), and Spike's libraries; `verif/regress/install-verilator.sh` and `verif/regress/install-spike.sh` install
both under `tools/`.

```sh
cd $CVA6_REPO_DIR
make verilate RISCV=/usr NUM_JOBS=16 verilator=/path/to/verilator-5.008/bin/verilator \
  SPIKE_INSTALL_DIR=$CVA6_REPO_DIR/tools/spike
```

Then run the whole suite, or one test, here. `QUICK=1` keeps the runs short.

```sh
make run SIM=cva6 QUICK=1 CVA6_REPO_DIR=/path/to/cva6
make run-t-drs SIM=cva6 QUICK=1 CVA6_REPO_DIR=/path/to/cva6
```

To check CVA6 against Spike instruction by instruction, run a test on CVA6, then `make trace-<test>`: it runs the same
ELF on Spike and prints the first instruction where the two disagree.

```sh
make run-t-drs SIM=cva6 QUICK=1 CVA6_REPO_DIR=/path/to/cva6
make trace-t-drs QUICK=1
```

### How a test runs

Each test is an M-mode monitor. It configures the domain state, then runs a stub in S or U mode with `run_lower(entry,
priv, mdstatus, a0, a1)`, which enters the stub through `mret`. That way the domain (`MPD`) and a pending entry
(`MPPEND`) come from the trap-return path.

The stub does one thing and then executes `ecall`. The first trap ends it: `run_lower` returns `mcause`, with the rest
of the trap, all registers included, in `last_trap`. A trap taken in M mode skips the faulting instruction by its
length, 2 or 4 bytes (the suite is built with C).

Tests tag memory only through `dom_reset()`, `dom_disable()` and `key_set()`, so they run unchanged with either key
source.

**`KEYSRC=pmp`**: `dom_reset()` builds a TOR chain over the 8 PMP entries.

| PMP entry | Region | Key |
|---|---|---|
| 0 | `[0, pages[1])`: code, data and page 0 | 0 |
| 1–6 | `pages[k]`, 4 KiB each | `k` |
| 7 | `pages[7]` and the two DRS pages | 7 |

All entries are RWX, so only the domain check can deny an access. The stacks, above the DRS, match no entry and are
out of reach of S and U mode. `dom_reset()` sets `dstatus = EN | DRSKEY=7` and leaves the other CSRs at their reset
values.

**`KEYSRC=sv39`**: `dom_reset()` builds an identity map of the image, the pages and the DRS with 4 KiB leaves, all
RWX, A and D set: the image has key 0, `pages[k]` key `k`, the DRS key 7, and the stacks are not mapped.

- S mode cannot fetch from U pages, so there are two copies of the tables, one with U leaves and one with S leaves;
  `run_lower()` sets `satp` for the stub's privilege.
- Both trees also map the image's 1 GiB region as one S page at `0xc0000000`, so that S mode can run image code while
  the U tables are active, as a trap handler delegated to S mode must (`keysrc_s_alias()`).
- `key_set(k, key)` retags only `pages[k]`.
- With `dstatus.EN = 0` the key bits are reserved again, so `dom_disable()` clears every key first.
- One PMP entry covers the map, and its key is ignored.

M mode never translates, so the locked-PMP checks in `t-acc` run only with `KEYSRC=pmp`.

## Linux user-space tests

They need a `qemu-riscv64` (linux-user) built from the `jitdomain` branch of the QEMU fork (`QEMU_USER=`) and
`riscv64-linux-gnu-gcc` (`LINUX_CC=`). They are built static, into `build/user/`.

```sh
make run-user QEMU_USER=/path/to/qemu-riscv64       # every user test
make run-u-enable QEMU_USER=/path/to/qemu-riscv64   # one test
```

Each test exits with code 1 when the CPU lacks `xjitdomain` (`QEMU_USER_CPU=`, default `rv64,xjitdomain=true`).

## Benchmarks

`make bench` builds and runs every benchmark in `bench/`, `make bench-<name>` one of them. They are not part of
`make run`, and their numbers only mean something with `SIM=cva6`; see [`bench/README.md`](bench/README.md).

```sh
make bench-switch SIM=cva6 CVA6_REPO_DIR=/path/to/cva6
```

## Layout

```
env/      bare-metal harness: boot, trap handler, CHECK_*, key sources, platforms, the .insn macros
tests/    one bare-metal test per t-<name>.c, with its stubs in t-<name>.S
user/     one Linux test per u-<name>.c, and their shared helpers in user.c
bench/    one benchmark per <name>.c, with its stubs in <name>.S
tools/    the CVA6 runner and the CVA6/Spike trace comparison
```

A new platform (the ZCU104, for instance) adds `env/platform/<name>/` with its `LOAD_ADDR`, `platform_putchar()` and
`exit()`; the tests do not change.

## Tests

One line per test. The comment at the top of each test file says what it checks in detail.

### Bare-metal tests

| Test | Checks |
|---|---|
| `t-acc` | the access check for every access kind, key and rights value |
| `t-typed` | domain-typed loads and stores, and ordinary accesses next to them |
| `t-csr` | reset values, field masks and access rules of the JITDomain CSRs |
| `t-pte` | keys from Sv39 PTEs (`KEYSRC=sv39` only) |
| `t-lpad` | `domcall` landing on a matching, aligned `domlpad` |
| `t-drs` | the domain return stack that `domcall` pushes and `domret` pops |
| `t-dsp` | `udsp`, DOM0's view of `dsp`, kept within `[dsp, dspbase]` |
| `t-trap` | traps and `xret` saving and restoring the domain and a pending entry |
| `t-rnmi` | the same for RNMIs and `mnret` (needs Smrnmi) |
| `t-flush` | the instruction right after a domain change using the new domain's rights |
| `t-spec` | no transient data from a denied load (needs cache timing, so CVA6 only) |
| `t-deputy` | which steps of a JIT exploit chain JITDomain stops |

### Linux user-space tests

| Test | Checks |
|---|---|
| `u-enable` | `PR_JITDOMAIN_ENABLE`, for the whole process and its threads |
| `u-pkey` | `pkey_alloc`, `pkey_free` and `pkey_mprotect` |
| `u-matrix` | `SET_MATRICES` and `GET_MATRICES`, and the new matrices reaching other threads |
| `u-access` | DOM0's accesses against the matrices and page keys |
| `u-domain` | DOM1-7's accesses, each against its own row |
| `u-drs` | `DRS_ALLOC`: a per-thread DRS between guard pages |
| `u-sigsys` | syscalls from DOM1-7 refused with `SIGSYS` |
| `u-fault` | what a fault tells the handler: `si_pkey`, `si_addr`, the `JDSG` record in the signal frame |
| `u-signal` | what `sigreturn` restores: the interrupted domain, its pending entry and `dsp`, or DOM0 |
