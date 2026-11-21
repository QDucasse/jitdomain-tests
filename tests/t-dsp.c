// t-dsp: udsp, the U-mode alias of dsp. Only DOM0 may access it, with
// dstatus.EN = 1, and a write must stay in [dsp, dspbase], so DOM0 can
// discard DRS records but never bring popped ones back.

#include "env.h"

STUB(udsp_r); STUB(udsp_rc0); STUB(udsp_rsi0); STUB(udsp_rci0);
STUB(udsp_rw); STUB(udsp_rs); STUB(udsp_rc);
STUB(udsp_rwi); STUB(udsp_rsi); STUB(udsp_rci);
STUB(s_dsp_w); STUB(reset_call); STUB(callback); STUB(ret_only);
STUB(callee); STUB(land);
extern char callback_restore[];

#define GATE(from, to) (1ULL << (8 * (from) + (to)))
#define CODE1 (PAGE(1) + 0x100)
#define TOP ((uintptr_t)drs_top)
#define SENTINEL 0x5a5a5a5aULL   // a1 on entry, the rd of every udsp access

static char *const reads[] = { udsp_r, udsp_rc0, udsp_rsi0, udsp_rci0 };
#define NREADS (sizeof(reads) / sizeof(reads[0]))

static uint64_t reg(int i)
{
  return last_trap.x[i];
}

// Record i from the top of the DRS
static volatile uint64_t *record(int i)
{
  return (volatile uint64_t *)(TOP - DRS_RECORD_SIZE * (i + 1));
}

// Every domain may fetch the stubs (key 0), DOM1 its callee in page 1, and
// DOM0 may domcall DOM1
static void setup(uint64_t dsp, uint64_t dspbase)
{
  dom_reset();
  for (int d = 0; d < NDOMS; d++)
    rights_set(d, 0, DR_X);
  rights_set(1, 1, DR_X);
  rights_apply();
  csr_write(CSR_DGATE, GATE(0, 1));
  csr_write(CSR_DSP, dsp);
  csr_write(CSR_DSPBASE, dspbase);
}

// The udsp access in stub ran: rd holds the old dsp, and dsp is now want
static void check_ok(char *stub, int dom, uint64_t a0, uint64_t want)
{
  uint64_t old = csr_read(CSR_DSP);
  CHECK_OK(run_u(stub, dom, a0, SENTINEL), PRV_U);
  CHECK_EQ(reg(11), old);
  CHECK_EQ(csr_read(CSR_DSP), want);
}

// The udsp access in stub raised a domain CFI fault with subcode sub, at its
// own pc, and changed neither dsp nor rd
static void check_fault(char *stub, int dom, uint64_t a0, uint64_t sub)
{
  uint64_t old = csr_read(CSR_DSP);
  CHECK_CFI_FAULT(run_u(stub, dom, a0, SENTINEL), sub);
  CHECK_EQ(last_trap.epc, (uintptr_t)stub);
  CHECK_EQ(last_trap.tval, (uintptr_t)stub);
  CHECK_EQ(reg(11), SENTINEL);
  CHECK_EQ(csr_read(CSR_DSP), old);
}

// dspbase resets to 0. Until S mode sets it, only a write of 0 from dsp = 0
// goes through, and a dsp set without dspbase cannot be moved at all.
static void check_reset_value(void)
{
  CHECK_EQ(csr_read(CSR_DSPBASE), 0);

  setup(0, 0);
  check_ok(udsp_r, 0, 0, 0);
  check_ok(udsp_rw, 0, 0, 0);
  check_fault(udsp_rw, 0, 8, DCAUSE_DSPRANGE);
  check_fault(udsp_rwi, 0, 0, DCAUSE_DSPRANGE);

  setup(TOP, 0);
  check_ok(udsp_r, 0, 0, TOP);
  check_fault(udsp_rw, 0, TOP, DCAUSE_DSPRANGE);
}

// udsp and dsp are the same register: DOM0 reads what S or M wrote to dsp,
// with any read form, and M reads dsp back after a udsp write
static void check_alias(void)
{
  static const uint64_t values[] = { 0, 8, TOP - 16, TOP, -8ULL };
  for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
    setup(values[i], TOP);
    for (unsigned r = 0; r < NREADS; r++)
      check_ok(reads[r], 0, 0, values[i]);
  }
  setup(TOP - 16, TOP);
  check_ok(udsp_rw, 0, TOP - 8, TOP - 8);
  CHECK_EQ(csr_read(CSR_DSP), TOP - 8);
}

// From any other domain, every access faults with DSP, before the range
// check: an in-range write, an out-of-range one, and a read
static void check_non_root(void)
{
  for (int d = 1; d < NDOMS; d++) {
    int before = failures;
    setup(TOP - 16, TOP);
    for (unsigned r = 0; r < NREADS; r++)
      check_fault(reads[r], d, 0, DCAUSE_DSP);
    check_fault(udsp_rw, d, TOP, DCAUSE_DSP);
    check_fault(udsp_rw, d, TOP + 8, DCAUSE_DSP);
    check_fault(udsp_rw, d, TOP - 24, DCAUSE_DSP);
    if (failures != before)
      printf("    (from DOM%d)\n", d);
  }
}

// With dstatus.EN = 0, every access is an illegal instruction, from any domain
static void check_disabled(void)
{
  setup(TOP - 16, TOP);
  dom_disable();
  for (int d = 0; d < 2; d++) {
    CHECK_EQ(run_u(udsp_r, d, 0, SENTINEL), CAUSE_ILLEGAL_INSN);
    CHECK_EQ(reg(11), SENTINEL);
    CHECK_EQ(run_u(udsp_rw, d, TOP, SENTINEL), CAUSE_ILLEGAL_INSN);
    CHECK_EQ(reg(11), SENTINEL);
    CHECK_EQ(csr_read(CSR_DSP), TOP - 16);
  }
}

// DOM0 writes with dsp = TOP - 16 and dspbase = TOP: anything in
// [dsp, dspbase] goes through, both ends included; anything below dsp (a
// replay attempt) or above dspbase faults. The value checked is the one the
// instruction would write, for every form.
static void check_range(void)
{
  const uint64_t dsp = TOP - 16;   // bit 3 clear, bit 4 set

  setup(dsp, TOP);
  check_ok(udsp_rw, 0, dsp, dsp);
  setup(dsp, TOP);
  check_ok(udsp_rw, 0, TOP - 8, TOP - 8);
  setup(dsp, TOP);
  check_ok(udsp_rw, 0, TOP, TOP);

  setup(dsp, TOP);
  check_fault(udsp_rw, 0, dsp - 8, DCAUSE_DSPRANGE);
  check_fault(udsp_rw, 0, dsp - 2 * PAGE_SIZE, DCAUSE_DSPRANGE);
  check_fault(udsp_rw, 0, TOP + 8, DCAUSE_DSPRANGE);
  check_fault(udsp_rw, 0, 0, DCAUSE_DSPRANGE);
  check_fault(udsp_rw, 0, -8ULL, DCAUSE_DSPRANGE);
  check_fault(udsp_rwi, 0, 0, DCAUSE_DSPRANGE);

  // csrrs: dsp | rs1. Setting bit 3 moves up by 8; a high bit passes dspbase.
  check_fault(udsp_rs, 0, 1ULL << 40, DCAUSE_DSPRANGE);
  check_ok(udsp_rs, 0, 8, TOP - 8);
  setup(dsp, TOP);
  check_ok(udsp_rsi, 0, 0, TOP - 8);

  // csrrc: dsp & ~rs1. Clearing bit 4 moves down; clearing a clear bit
  // writes dsp itself, which goes through.
  setup(dsp, TOP);
  check_fault(udsp_rc, 0, 0x10, DCAUSE_DSPRANGE);
  check_ok(udsp_rc, 0, 8, dsp);
  check_ok(udsp_rci, 0, 0, dsp);
  setup(TOP - 8, TOP);
  check_fault(udsp_rci, 0, 0, DCAUSE_DSPRANGE);
}

// S mode moves dsp above dspbase, unchecked. DOM0 can still read it, with
// every read form, but every write faults, even one of dsp itself or of
// dspbase. A register source of 0 still writes, unlike x0.
static void check_above_base(void)
{
  setup(TOP - 16, TOP);
  CHECK_OK(run_s(s_dsp_w, 0, TOP + 8, 0), PRV_S);
  CHECK_EQ(csr_read(CSR_DSP), TOP + 8);

  for (unsigned r = 0; r < NREADS; r++)
    check_ok(reads[r], 0, 0, TOP + 8);
  check_fault(udsp_rw, 0, TOP + 8, DCAUSE_DSPRANGE);
  check_fault(udsp_rw, 0, TOP, DCAUSE_DSPRANGE);
  check_fault(udsp_rs, 0, 0, DCAUSE_DSPRANGE);
  check_fault(udsp_rc, 0, 0, DCAUSE_DSPRANGE);
}

// DOM0 discards two stale records by resetting udsp to the top, then
// makes a round trip into DOM1: the push lands in the top slot, and the pop
// brings dsp back to the top
static void check_push_pop_after_reset(void)
{
  setup(TOP - 2 * DRS_RECORD_SIZE, TOP);
  place_stub(CODE1, callee, callee_end);
  *record(0) = DRS_RECORD(0x1000, 1);
  *record(1) = DRS_RECORD(0x2000, 1);

  CHECK_OK(run_u(reset_call, 0, CODE1, TOP), PRV_U);
  CHECK_EQ(reg(19), 1);   // s3, in the callee
  CHECK_EQ(reg(18), 0);   // s2, back in the caller
  CHECK_EQ(last_trap.epc, (uintptr_t)reset_call + 12);
  CHECK_EQ(csr_read(CSR_DSP), TOP);
  CHECK_EQ(*record(0), DRS_RECORD((uintptr_t)reset_call + 8, 0));
}

// A reset to a misaligned value is accepted, and the next push faults with DRS
static void check_misaligned_reset(void)
{
  setup(TOP - 16, TOP);
  place_stub(CODE1, callee, callee_end);
  CHECK_CFI_FAULT(run_u(reset_call, 0, CODE1, TOP - 12), DCAUSE_DRS);
  CHECK_EQ(last_trap.epc, (uintptr_t)reset_call + 4);
  CHECK_EQ(csr_read(CSR_DSP), TOP - 12);
}

// Nested bases. DOM1 has called into DOM0 (an FFI gate), whose record R1
// returns to DOM1 at land; the callback then enters DOM0 again on top of it.
// - Resetting only to its own base (the dsp it entered with), the callback's
//   own domcall lands below R1, the restore goes through, and DOM0's later
//   domret still returns through R1, into DOM1.
// - Resetting to the top of the DRS, past its base, the callback's domcall
//   overwrites R1, and the restore of its base, below dsp, faults with
//   DSPRANGE instead of exposing the overwritten slot.
static void check_nested_bases(void)
{
  const uint64_t r1 = DRS_RECORD((uintptr_t)land, 1);

  setup(TOP - DRS_RECORD_SIZE, TOP);
  place_stub(CODE1, callee, callee_end);
  *record(0) = r1;
  CHECK_OK(run_u(callback, 0, CODE1, 0), PRV_U);
  CHECK_EQ(reg(19), 1);
  CHECK_EQ(csr_read(CSR_DSP), TOP - DRS_RECORD_SIZE);
  CHECK_EQ(*record(0), r1);
  CHECK_EQ(*record(1), DRS_RECORD((uintptr_t)callback_restore, 0));
  CHECK_OK(run_u(ret_only, 0, 0, 0), PRV_U);
  CHECK_EQ(last_trap.epc, (uintptr_t)land + 4);
  CHECK_EQ(reg(18), 1);   // s2, at land
  CHECK_EQ(csr_read(CSR_DSP), TOP);

  setup(TOP - DRS_RECORD_SIZE, TOP);
  *record(0) = r1;
  CHECK_CFI_FAULT(run_u(callback, 0, CODE1, TOP), DCAUSE_DSPRANGE);
  CHECK_EQ(reg(19), 1);
  CHECK_EQ(last_trap.epc, (uintptr_t)callback_restore);
  CHECK_EQ(last_trap.tval, (uintptr_t)callback_restore);
  CHECK_EQ(csr_read(CSR_DSP), TOP);
  CHECK_EQ(*record(0), DRS_RECORD((uintptr_t)callback_restore, 0));
}

int main(void)
{
  check_reset_value();
  check_alias();
  check_non_root();
  check_disabled();
  check_range();
  check_above_base();
  check_push_pop_after_reset();
  check_misaligned_reset();
  check_nested_bases();
  return report("t-dsp");
}
