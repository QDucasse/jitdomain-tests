# Benchmarks

One ELF per benchmark, built from `<name>.c` and its stubs in `<name>.S`. `make bench` runs them all, `make
bench-<name>` one of them. The cycle counts only mean something on the RTL; on Spike and QEMU, a benchmark only checks
that every stub runs.

## switch

Cycles per domain transition, next to an ordinary call, a trap round trip and ordinary and typed memory accesses.

```sh
make bench-switch SIM=cva6 CVA6_REPO_DIR=/path/to/cva6
make bench-switch SIM=cva6 KEYSRC=sv39 CVA6_REPO_DIR=/path/to/cva6
```

Each stub runs in U mode and reads `cycle` and `instret` around the code it times. DOM0 runs from the image (key 0),
DOM1 from page 1 (key 1).

Steady-state costs: a loop repeats one operation 1000 times, once to warm the caches and once measured. The empty
loop is subtracted, and the cost is printed per operation.

| Line | Operation |
|---|---|
| `jal/ret call` | an ordinary call to an empty function |
| round trip DOM0 -> DOM1 -> DOM0 | `domcall`, `domlpad`, `domret` |
| round trip DOM0 -> DOM0 | the same without changing domain |
| one-way domcall | DOM0 and DOM1 domcall each other without returning; per `domcall` |
| trap round trip | `ecall` to a minimal M-mode handler that `mret`s at once |
| `ld`, `dld`, `sd`, `dsd` | ordinary and typed accesses, per access |

Cold costs: two round trips DOM0 -> DOM1 -> DOM0 timed one by one, five times after a data cache eviction and five
times without. The harness flushes the TLB on every stub entry, so with `KEYSRC=sv39` the first round trip also pays
its TLB misses. Each line prints the min-max range of the first and the second round trip.

`switch.S` holds the stubs (loops, round-trip callees, single-shot round trips, the M-mode handler of the trap loop),
`switch.c` the domain setup, measurement and report.
