// Switch-cost benchmark: cycles per domain transition, next to an ordinary
// call, a trap round trip and ordinary and typed memory accesses.

#include "env.h"

STUB(loop_empty); STUB(loop_call);
STUB(loop_round_trip_dom1); STUB(loop_round_trip_dom0);
STUB(callee_dom1); STUB(callee_dom0);
STUB(loop_one_way); STUB(enter_dom0);
STUB(loop_ld); STUB(loop_dld); STUB(loop_sd); STUB(loop_dsd);
STUB(loop_trap); STUB(single_empty); STUB(single_round_trips);
extern char trap_return[];

// Where trap_return sends the traps it does not handle
uint64_t harness_trap_vector;

#define ITERATIONS 1000
// Two DRS records per iteration, in a DRS of 1024 records
#define ONE_WAY_ITERATIONS 200
#define SHOTS 5

#define CODE1 (PAGE(1) + 0x100)
#define DATA_PAGE 2
#define GATE(from, to) (1ULL << (8 * (from) + (to)))
#define CAUSE_BREAKPOINT 3

// Registers of the stub at its last trap
#define S0 8
#define S1 9
#define S2 18
#define S3 19

// Read well over the data cache's size from memory past the image
#define EVICT_BYTES (256 * 1024)
#define EVICT_STRIDE 16
extern char _end[];

static void evict(void)
{
  uintptr_t base = ((uintptr_t)_end + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1);
  uint64_t sum = 0;
  for (uintptr_t address = base; address < base + EVICT_BYTES; address += EVICT_STRIDE)
    sum += *(volatile uint64_t *)address;
  __asm__ volatile("" :: "r"(sum));
}

// DOM0 runs the stubs from the image (key 0) and reads and writes the data
// page, DOM1 runs the snippets copied into page 1 (key 1); DOM0 may domcall
// DOM0 and DOM1, and DOM1 may domcall DOM0. The DRS is empty.
static void setup(void)
{
  dom_reset();
  rights_set(0, 0, DR_X);
  rights_set(0, DATA_PAGE, DR_R | DR_W);
  rights_set(1, 1, DR_X);
  rights_apply();
  csr_write(CSR_DGATE, GATE(0, 0) | GATE(0, 1) | GATE(1, 0));
  csr_write(CSR_DSP, (uintptr_t)drs_top);
}

typedef struct {
  int64_t cycles, instret;
} cost_t;

// Run a loop stub in DOM0 and return what its loop took. The first run warms
// the caches, the second one is measured.
static cost_t run_loop(void *stub, uint64_t iterations, uint64_t arg, uint64_t expected_cause)
{
  for (int run = 0; run < 2; run++) {
    csr_write(CSR_DSP, (uintptr_t)drs_top);
    CHECK_EQ(run_u(stub, 0, iterations, arg), expected_cause);
    CHECK_EQ(last_trap.mdstatus, 0);
  }
  return (cost_t){
    (int64_t)(last_trap.x[S1] - last_trap.x[S0]),
    (int64_t)(last_trap.x[S3] - last_trap.x[S2]),
  };
}

// value / divisor with two decimals
static void print_ratio(int64_t value, int64_t divisor)
{
  int64_t hundredths = (value * 100 + (value < 0 ? -divisor : divisor) / 2) / divisor;
  if (hundredths < 0) {
    printf("-");
    hundredths = -hundredths;
  }
  printf("%d.%d%d", hundredths / 100, (hundredths / 10) % 10, hundredths % 10);
}

// One line: the cost of an operation that a loop of iterations repeats
// operations times per iteration, beyond the empty loop
static void report_loop(const char *name, cost_t cost, cost_t empty, int64_t iterations, int64_t operations)
{
  int64_t empty_cycles = empty.cycles * iterations / ITERATIONS;
  int64_t empty_instret = empty.instret * iterations / ITERATIONS;
  printf("  %s: ", name);
  print_ratio(cost.cycles - empty_cycles, iterations * operations);
  printf(" cycles, ");
  print_ratio(cost.instret - empty_instret, iterations * operations);
  printf(" instructions\n");
}

// Steady-state costs: each loop stub repeats one operation in U mode with
// everything warm, and the empty loop is subtracted
static void bench_loops(void)
{
  setup();
  place_stub(CODE1, callee_dom1, callee_dom1_end);
  cost_t empty = run_loop(loop_empty, ITERATIONS, 0, CAUSE_ECALL_U);

  printf("per operation, %d iterations, beyond the empty loop:\n", ITERATIONS);
  report_loop("jal/ret call", run_loop(loop_call, ITERATIONS, 0, CAUSE_ECALL_U), empty, ITERATIONS, 1);

  report_loop("round trip DOM0 -> DOM1 -> DOM0 (domcall, domlpad, domret)",
              run_loop(loop_round_trip_dom1, ITERATIONS, CODE1, CAUSE_ECALL_U), empty, ITERATIONS, 1);
  CHECK_EQ(csr_read(CSR_DSP), (uintptr_t)drs_top);
  report_loop("round trip DOM0 -> DOM0",
              run_loop(loop_round_trip_dom0, ITERATIONS, (uintptr_t)callee_dom0, CAUSE_ECALL_U), empty, ITERATIONS, 1);
  CHECK_EQ(csr_read(CSR_DSP), (uintptr_t)drs_top);

  place_stub(CODE1, enter_dom0, enter_dom0_end);
  report_loop("one-way domcall (DOM0 -> DOM1 -> DOM0, per domcall)",
              run_loop(loop_one_way, ONE_WAY_ITERATIONS, CODE1, CAUSE_ECALL_U), empty, ONE_WAY_ITERATIONS, 2);
  CHECK_EQ(csr_read(CSR_DSP), (uintptr_t)drs_top - 2 * DRS_RECORD_SIZE * ONE_WAY_ITERATIONS);

  harness_trap_vector = csr_read(mtvec);
  csr_write(mtvec, (uintptr_t)trap_return);
  cost_t trap = run_loop(loop_trap, ITERATIONS, 0, CAUSE_BREAKPOINT);
  csr_write(mtvec, harness_trap_vector);
  report_loop("trap round trip (ecall to M mode, mret)", trap, empty, ITERATIONS, 1);

  report_loop("ld", run_loop(loop_ld, ITERATIONS, PAGE(DATA_PAGE), CAUSE_ECALL_U), empty, ITERATIONS, 8);
  report_loop("dld", run_loop(loop_dld, ITERATIONS, PAGE(DATA_PAGE), CAUSE_ECALL_U), empty, ITERATIONS, 8);
  report_loop("sd", run_loop(loop_sd, ITERATIONS, PAGE(DATA_PAGE), CAUSE_ECALL_U), empty, ITERATIONS, 8);
  report_loop("dsd", run_loop(loop_dsd, ITERATIONS, PAGE(DATA_PAGE), CAUSE_ECALL_U), empty, ITERATIONS, 8);
}

// Cold costs: cycles between the first and second, and the second and third
// cycle reads of single_round_trips, minus those of single_empty. Each run
// starts with or without a data cache eviction. run_lower() also flushes the
// TLB on every stub entry, so with Sv39 keys the first round trip pays its
// TLB misses too.
static void bench_single(void)
{
  setup();
  place_stub(CODE1, callee_dom1, callee_dom1_end);
  CHECK_OK(run_u(single_empty, 0, 0, 0), PRV_U);
  CHECK_OK(run_u(single_empty, 0, 0, 0), PRV_U);
  int64_t empty_first = last_trap.x[S1] - last_trap.x[S0];
  int64_t empty_second = last_trap.x[S2] - last_trap.x[S1];

  printf("single round trips DOM0 -> DOM1 -> DOM0, %d runs:\n", SHOTS);
  for (int evicted = 1; evicted >= 0; evicted--) {
    int64_t first_min = INT64_MAX, first_max = 0, second_min = INT64_MAX, second_max = 0;
    for (int shot = 0; shot < SHOTS; shot++) {
      csr_write(CSR_DSP, (uintptr_t)drs_top);
      if (evicted)
        evict();
      CHECK_OK(run_u(single_round_trips, 0, 0, CODE1), PRV_U);
      int64_t first = (int64_t)(last_trap.x[S1] - last_trap.x[S0]) - empty_first;
      int64_t second = (int64_t)(last_trap.x[S2] - last_trap.x[S1]) - empty_second;
      first_min = first < first_min ? first : first_min;
      first_max = first > first_max ? first : first_max;
      second_min = second < second_min ? second : second_min;
      second_max = second > second_max ? second : second_max;
    }
    printf("  %s: first %d-%d cycles, second %d-%d cycles\n",
           evicted ? "after a data cache eviction" : "without eviction",
           first_min, first_max, second_min, second_max);
  }
}

// The cycle counts only mean something on the RTL; on Spike and QEMU this
// only checks that every stub runs
int main(void)
{
  // Let U mode read cycle and instret
  csr_write(mcounteren, 0x5);
  csr_write(scounteren, 0x5);

#if KEYSRC_SV39
  printf("switch: keys from Sv39 PTEs (the TLB is flushed at every stub entry)\n");
#else
  printf("switch: keys from PMP entries\n");
#endif
  bench_loops();
  bench_single();
  return report("switch");
}
