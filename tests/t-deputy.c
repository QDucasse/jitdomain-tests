// T-DEPUTY: what JITDomain stops in a JIT exploit chain, and what it does not.
// The chain follows NoJITsu's SpiderMonkey exploit (CVE-2019-11707): an
// out-of-bounds write from a heap array overwrites the data pointer of the
// next ByteArray, and the runtime's at:/at:put: primitives then read and write
// wherever it points, with the runtime's rights.
//
// Every step runs with dstatus.EN = 0, where it must succeed, then with
// EN = 1. With EN = 1 the steps JITDomain stops must raise a domain access
// fault on the expected address, and the others must still succeed. The test
// prints both outcomes and who is responsible for each step.
//
// The configuration is the Cog one: K0 = runtime (the image: DOM0 code and
// runtime data), K1 = heap (page 1), K2 = DOM2 table (page 2), K3 = code
// zone (page 3). DOM1 has RW on K1 and RX on K3, DOM2 RW on K2, DOM0
// everything. dtarget[1] = 2 lets JIT code reach the table with typed
// accesses, and dtarget[0] = 1 lets the runtime follow heap pointers with
// DOM1's rights.
//
// The steps after the first start from a heap that the out-of-bounds write
// has already corrupted, so that each one shows a single effect. The
// runtime's index and tag checks on table handles are software, and are not
// checked here.

#include "env.h"

STUB(jit_store); STUB(jit_typed_store);
STUB(jit_call_1); STUB(jit_call_2); STUB(jit_call_3);
extern char prim_at[], prim_at_typed[], prim_at_put[], prim_at_put_typed[];
extern char prim_at_handle[];

#define GATE(from, to) (1ULL << (8 * (from) + (to)))
#define HEAP PAGE(1)
#define TABLE PAGE(2)
#define CODE PAGE(3)

#define TABLE_TAG_BYTES 0x42

// The heap: the JIT frame, an array of 4 words followed by a ByteArray, a
// legitimate backing store, and a handle into the table
#define FRAME ((volatile uint64_t *)(HEAP + 0x000))
#define ARR (HEAP + 0x100)
#define BA ((volatile uint64_t *)(HEAP + 0x120))   // {data, len}
#define BYTES ((volatile uint8_t *)(HEAP + 0x200))
#define HANDLE ((volatile uint64_t *)(HEAP + 0x300))   // {idx}
#define ENTRY(i) ((volatile uint64_t *)(TABLE + 24 * (i)))   // {ptr, len, tag}

// Runtime data, K0
#define WORD_ALIGNED __attribute__((aligned(8)))
static volatile uint8_t secret[8] WORD_ALIGNED = { 0x5e };
static volatile uint8_t runtime_var[8] WORD_ALIGNED;
static volatile uint8_t buf_a[8] = { 0xaa };
static volatile uint8_t buf_b[8] = { 0xbb };
uintptr_t deputy_table = TABLE;

// Where the JIT snippets are placed in the code zone
static uintptr_t jit_store_at, jit_typed_store_at, jit_call_at[4];

enum { SUCCEEDED, STOPPED, BROKEN };
static const char *const outcome_name[] = { "succeeds", "stopped", "BROKEN" };

static uintptr_t place(uintptr_t *at, uintptr_t dst, const char *start, const char *end)
{
  *at = (uintptr_t)place_stub(dst, start, end);
  return dst + 0x40;
}

// The Cog configuration, with the heap, table and runtime data reset, and
// JITDomain enabled or not
static void setup(bool enabled)
{
  dom_reset();
  rights_set(0, 0, DR_R | DR_W | DR_X);
  for (int k = 1; k <= 3; k++)
    rights_set(0, k, DR_R | DR_W);
  rights_set(1, 1, DR_R | DR_W);
  rights_set(1, 3, DR_R | DR_X);
  rights_set(2, 2, DR_R | DR_W);
  rights_apply();
  uint64_t dtarget = dtarget_identity() & ~0x3fULL;
  csr_write(CSR_DTARGET, dtarget | (1 << 0) | (2 << 3));
  csr_write(CSR_DGATE, GATE(0, 1) | GATE(1, 0));
  csr_write(CSR_DSP, (uintptr_t)drs_top);

  uintptr_t at = CODE;
  at = place(&jit_store_at, at, jit_store, jit_store_end);
  at = place(&jit_typed_store_at, at, jit_typed_store, jit_typed_store_end);
  at = place(&jit_call_at[1], at, jit_call_1, jit_call_1_end);
  at = place(&jit_call_at[2], at, jit_call_2, jit_call_2_end);
  place(&jit_call_at[3], at, jit_call_3, jit_call_3_end);

  memset((void *)HEAP, 0, PAGE_SIZE);
  BA[0] = (uintptr_t)BYTES;
  BA[1] = 8;
  BYTES[0] = 0x11;
  HANDLE[0] = 0;
  ENTRY(0)[0] = (uintptr_t)buf_a;
  ENTRY(0)[1] = sizeof(buf_a);
  ENTRY(0)[2] = TABLE_TAG_BYTES;
  ENTRY(1)[0] = (uintptr_t)buf_b;
  ENTRY(1)[1] = sizeof(buf_b);
  ENTRY(1)[2] = TABLE_TAG_BYTES;
  runtime_var[0] = 0;

  if (!enabled)
    dom_disable();
}

// Run JIT code in DOM1 with the frame {a0, a1, a2, target}
static uint64_t jit(uintptr_t code, uint64_t a0, uint64_t a1, uint64_t a2, void *target)
{
  FRAME[0] = a0;
  FRAME[1] = a1;
  FRAME[2] = a2;
  FRAME[3] = (uintptr_t)target;
  return run_u((void *)code, 1, (uintptr_t)FRAME, 0);
}

// The step succeeded if it ended normally with its effect visible, and was
// stopped if it faulted on addr without the effect
static int outcome(uint64_t cause, bool effect, uintptr_t addr)
{
  if (cause == CAUSE_ECALL_U)
    return effect ? SUCCEEDED : BROKEN;
  if (cause == CAUSE_DOMAIN_ACCESS && last_trap.tval == addr && !effect)
    return STOPPED;
  return BROKEN;
}

// Stage 1: the out-of-bounds write. Inside the heap it rewrites the next
// object's data pointer; far enough, it reaches runtime data.
static int oob_in_heap(void)
{
  uint64_t cause = jit(jit_store_at, ARR, 4, (uintptr_t)secret, 0);
  return outcome(cause, BA[0] == (uintptr_t)secret, BA[0]);
}

static int oob_to_runtime(void)
{
  uintptr_t target = (uintptr_t)runtime_var;
  uint64_t cause = jit(jit_store_at, ARR, (int64_t)(target - ARR) / 8, 0x77, 0);
  return outcome(cause, runtime_var[0] == 0x77, target);
}

// The legitimate call still works when the runtime uses typed accesses
static int normal_typed(void)
{
  uint64_t cause = jit(jit_call_at[1], (uintptr_t)BA, 0, 0, prim_at_typed);
  return outcome(cause, last_trap.a0 == 0x11, 0);
}

// Stage 2: the forged ByteArray handed to the runtime
static int deputy_leak(void *prim)
{
  BA[0] = (uintptr_t)secret;
  uint64_t cause = jit(jit_call_at[1], (uintptr_t)BA, 0, 0, prim);
  return outcome(cause, cause == CAUSE_ECALL_U && last_trap.a0 == secret[0], (uintptr_t)secret);
}

static int deputy_overwrite(void *prim)
{
  BA[0] = (uintptr_t)runtime_var;
  uint64_t cause = jit(jit_call_at[2], (uintptr_t)BA, 0, 0x77, prim);
  return outcome(cause, runtime_var[0] == 0x77, (uintptr_t)runtime_var);
}

static int deputy_leak_ordinary(void) { return deputy_leak(prim_at); }
static int deputy_leak_typed(void) { return deputy_leak(prim_at_typed); }
static int deputy_overwrite_ordinary(void) { return deputy_overwrite(prim_at_put); }
static int deputy_overwrite_typed(void) { return deputy_overwrite(prim_at_put_typed); }

// Stage 3: handles into the DOM2 table instead of raw pointers. A handle
// swapped for another valid one of the same type still works.
static int handle_swap(void)
{
  HANDLE[0] = 1;
  uint64_t cause = jit(jit_call_at[3], (uintptr_t)HANDLE, 0, 0, prim_at_handle);
  return outcome(cause, cause == CAUSE_ECALL_U && last_trap.a0 == buf_b[0], 0);
}

// An ordinary store from JIT code cannot rewrite the table; a typed one
// (dtarget[1] = 2) can
static int table_store_ordinary(void)
{
  uint64_t cause = jit(jit_store_at, (uintptr_t)ENTRY(0), 0, (uintptr_t)secret, 0);
  return outcome(cause, ENTRY(0)[0] == (uintptr_t)secret, (uintptr_t)ENTRY(0));
}

static int table_store_typed(void)
{
  uint64_t cause = jit(jit_typed_store_at, (uintptr_t)ENTRY(0), 0, (uintptr_t)secret, 0);
  return outcome(cause, ENTRY(0)[0] == (uintptr_t)secret, (uintptr_t)ENTRY(0));
}

static const struct {
  const char *name;
  int (*run)(void);
  bool stopped;   // by JITDomain, with EN = 1
  const char *owner;
} steps[] = {
  { "OOB write into the next heap object", oob_in_heap, false, "runtime and JIT compiler" },
  { "OOB write into runtime data", oob_to_runtime, true, "JITDomain" },
  { "normal at:, typed deref", normal_typed, false, "-" },
  { "deputy leak, ordinary deref", deputy_leak_ordinary, false, "runtime: typed deref or DOM2 table" },
  { "deputy overwrite, ordinary deref", deputy_overwrite_ordinary, false, "runtime: typed deref or DOM2 table" },
  { "deputy leak, typed deref", deputy_leak_typed, true, "JITDomain, with the runtime's typed deref" },
  { "deputy overwrite, typed deref", deputy_overwrite_typed, true, "JITDomain, with the runtime's typed deref" },
  { "handle swapped for one of the same type", handle_swap, false, "accepted by design" },
  { "ordinary store to the DOM2 table", table_store_ordinary, true, "JITDomain" },
  { "typed store to the DOM2 table", table_store_typed, false, "residual-surface audit" },
};

static void column(const char *s, int width)
{
  int n = 0;
  for (; s[n]; n++)
    platform_putchar(s[n]);
  for (; n < width; n++)
    platform_putchar(' ');
}

int main(void)
{
  column("step", 42);
  column("EN=0", 10);
  column("EN=1", 10);
  printf("owner\n");
  for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
    setup(false);
    int off = steps[i].run();
    setup(true);
    int on = steps[i].run();
    CHECK_EQ(off, SUCCEEDED);
    CHECK_EQ(on, steps[i].stopped ? STOPPED : SUCCEEDED);

    column(steps[i].name, 42);
    column(outcome_name[off], 10);
    column(outcome_name[on], 10);
    printf("%s\n", steps[i].owner);
  }
  return report("t-deputy");
}
