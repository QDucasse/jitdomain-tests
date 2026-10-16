// T-LPAD: domcall must land on a matching, 4-byte aligned domlpad. Covers a
// non-pad target, a pad with the wrong domain or the wrong non-zero label,
// wildcard pads, a 2-byte aligned pad, the pad's fetch rights, the gate,
// a domcall into the caller's own domain, a pad reached without domcall,
// plain jumps and branches into another domain's code, and Zicfilp's ELP.
//
// Callers (t-lpad.S) run in DOM0 from the image, key 0. Pads are copied into
// page k, key k. A pad records curdom in s2 and ends with an ecall.

#include "env.h"

STUB(call_0_5); STUB(call_1_5); STUB(call_1_0); STUB(jalr_a0);
STUB(pad_0_5); STUB(pad_1_5); STUB(pad_1_0); STUB(pad_1_6); STUB(pad_2_5);
STUB(nopad); STUB(pad_1_5_at2); STUB(jal_fwd); STUB(beq_fwd);

#define GATE(from, to) (1ULL << (8 * (from) + (to)))
#define TARGET (PAGE(1) + 0x100)

#define CAUSE_SOFTWARE_CHECK 18
#define SWCHECK_LANDING_PAD 2
#define CSR_SENVCFG 0x10a
#define SENVCFG_LPE (1ULL << 2)
#define MSTATUS_MPELP (1ULL << 41)

static uint64_t s2(void)
{
  return last_trap.x[18];
}

// Word i of the first record pushed on an empty DRS: 0 = pc, 1 = domain
static uint64_t drs_first(int i)
{
  return *(volatile uint64_t *)((uintptr_t)drs_top - 16 + 8 * i);
}

// DOM0 runs the callers from key 0 and DOM1 its pads from key 1. DOM0 may
// domcall DOM1, and the DRS starts empty.
static void setup(void)
{
  dom_reset();
  rights_set(0, 0, DR_X);
  rights_set(1, 1, DR_X);
  rights_apply();
  csr_write(CSR_DGATE, GATE(0, 1));
  csr_write(CSR_DSP, (uintptr_t)drs_top);
}

// A successful domcall from DOM0: the pad ran in DOM1 and the DRS holds one
// record {caller + 4, DOM0}
static void check_entered(uint64_t cause, char *caller, int dom)
{
  CHECK_OK(cause, PRV_U);
  CHECK_EQ(s2(), dom);
  CHECK_EQ(last_trap.mdstatus, (uint64_t)dom << DSTATUS_XPD_SHIFT);
  CHECK_EQ(last_trap.x[1], (uintptr_t)caller + 4);
  CHECK_EQ(csr_read(CSR_DSP), (uintptr_t)drs_top - 16);
  CHECK_EQ(drs_first(0), (uintptr_t)caller + 4);
  CHECK_EQ(drs_first(1), 0);
}

// A domcall that completed but found no matching pad at target: an LPAD fault
// with the entry still pending
static void check_lpad_fault(uint64_t cause, uintptr_t target, int dom, int label)
{
  CHECK_CFI_FAULT(cause, DCAUSE_LPAD);
  CHECK_EQ(last_trap.epc, target);
  CHECK_EQ(last_trap.tval, target);
  CHECK_EQ(last_trap.mdstatus, (uint64_t)PEND(dom, label) << DSTATUS_XPPEND_SHIFT);
  CHECK_EQ(csr_read(CSR_DSP), (uintptr_t)drs_top - 16);
}

static void check_match(void)
{
  setup();
  place_stub(TARGET, pad_1_5, pad_1_5_end);
  check_entered(run_u(call_1_5, 0, TARGET, 0), call_1_5, 1);

  // rs1 bit 0 is ignored, as for jalr
  setup();
  check_entered(run_u(call_1_5, 0, TARGET | 1, 0), call_1_5, 1);

  // A wildcard pad accepts any label, including 0
  setup();
  place_stub(TARGET, pad_1_0, pad_1_0_end);
  check_entered(run_u(call_1_5, 0, TARGET, 0), call_1_5, 1);
  setup();
  check_entered(run_u(call_1_0, 0, TARGET, 0), call_1_0, 1);
}

static void check_mismatch(void)
{
  static const struct {
    const char *name;
    char *pad, *pad_end;
    char *caller;
    int label;
  } cases[] = {
    { "no pad",          nopad,   nopad_end,   call_1_5, 5 },
    { "wrong domain",    pad_2_5, pad_2_5_end, call_1_5, 5 },
    { "wrong label",     pad_1_6, pad_1_6_end, call_1_5, 5 },
    { "label-0 call",    pad_1_5, pad_1_5_end, call_1_0, 0 },
  };

  for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    setup();
    place_stub(TARGET, cases[i].pad, cases[i].pad_end);
    int before = failures;
    check_lpad_fault(run_u(cases[i].caller, 0, TARGET, 0), TARGET, 1, cases[i].label);
    if (failures != before)
      printf("    (%s)\n", cases[i].name);
  }

  // A matching pad that is only 2-byte aligned
  setup();
  place_stub(TARGET, pad_1_5_at2, pad_1_5_at2_end);
  check_lpad_fault(run_u(call_1_5, 0, TARGET + 2, 0), TARGET + 2, 1, 5);
}

// The pad is fetched as the pending domain: DOM0's X on key 1 does not help
static void check_pad_fetch(void)
{
  setup();
  rights_set(1, 1, 0);
  rights_set(0, 1, DR_X);
  rights_apply();
  place_stub(TARGET, pad_1_5, pad_1_5_end);
  uint64_t cause = run_u(call_1_5, 0, TARGET, 0);
  CHECK_DOM_FAULT(cause, TARGET);
  CHECK_EQ(last_trap.mdstatus, (uint64_t)PEND(1, 5) << DSTATUS_XPPEND_SHIFT);
}

// The gate row is the caller's: DOM1 may call DOM1, but DOM0 may not. A
// denied domcall faults on itself and changes nothing.
static void check_gate(void)
{
  setup();
  csr_write(CSR_DGATE, GATE(1, 1));
  place_stub(TARGET, pad_1_5, pad_1_5_end);
  uint64_t cause = run_u(call_1_5, 0, TARGET, 0);
  CHECK_CFI_FAULT(cause, DCAUSE_GATE);
  CHECK_EQ(last_trap.epc, (uintptr_t)call_1_5);
  CHECK_EQ(last_trap.tval, (uintptr_t)call_1_5);
  CHECK_EQ(last_trap.mdstatus, 0);
  CHECK_EQ(last_trap.x[1], 0);
  CHECK_EQ(csr_read(CSR_DSP), (uintptr_t)drs_top);

  // A domcall into the caller's own domain needs its gate bit and a pad
  setup();
  place_stub(PAGE(0) + 0x100, pad_0_5, pad_0_5_end);
  CHECK_CFI_FAULT(run_u(call_0_5, 0, PAGE(0) + 0x100, 0), DCAUSE_GATE);
  csr_write(CSR_DGATE, GATE(0, 0));
  check_entered(run_u(call_0_5, 0, PAGE(0) + 0x100, 0), call_0_5, 0);
}

// Without a pending entry a domlpad is a no-op, and ordinary jumps and
// branches into another domain's code fetch and run it as the current domain
static void check_no_domcall(void)
{
  setup();
  place_stub(TARGET, pad_1_5, pad_1_5_end);
  CHECK_OK(run_u((void *)TARGET, 1, 0, 0), PRV_U);
  CHECK_EQ(s2(), 1);
  CHECK_DOM_FAULT(run_u((void *)TARGET, 2, 0, 0), TARGET);

  // DOM0 jumps to DOM1's pad: it runs as a no-op in DOM0, or is not fetched
  // at all without DOM0's X on key 1
  CHECK_DOM_FAULT(run_u(jalr_a0, 0, TARGET, 0), TARGET);
  rights_set(0, 1, DR_X);
  rights_apply();
  CHECK_OK(run_u(jalr_a0, 0, TARGET, 0), PRV_U);
  CHECK_EQ(s2(), 0);
  CHECK_EQ(last_trap.mdstatus, 0);

  // jal and beq from page 1 into page 2, in DOM1
  static const struct { char *start, *end; } jumps[] = {
    { jal_fwd, jal_fwd_end },
    { beq_fwd, beq_fwd_end },
  };
  uintptr_t from = PAGE(1) + 0xc00, to = from + 0x500;
  place_stub(to, nopad, nopad_end);
  for (unsigned i = 0; i < 2; i++) {
    setup();
    place_stub(from, jumps[i].start, jumps[i].end);
    CHECK_DOM_FAULT(run_u((void *)from, 1, 0, 0), to);
    rights_set(1, 2, DR_X);
    rights_apply();
    CHECK_OK(run_u((void *)from, 1, 0, 0), PRV_U);
    CHECK_EQ(s2(), 1);
  }
}

// Zicfilp, when implemented: domlpad does not satisfy ELP, and domcall does not
// set it
static void check_zicfilp(void)
{
  uint64_t before = m_traps;
  csr_set(CSR_SENVCFG, SENVCFG_LPE);
  bool lpe = m_traps == before && (csr_read(CSR_SENVCFG) & SENVCFG_LPE);
  if (!lpe) {
    printf("t-lpad: no Zicfilp, ELP checks skipped\n");
    return;
  }

  setup();
  rights_set(0, 1, DR_X);
  rights_apply();

  // The trap saved ELP in MPELP; clear it so the next stub starts without it
  place_stub(TARGET, nopad, nopad_end);
  CHECK_EQ(run_u(jalr_a0, 0, TARGET, 0), CAUSE_SOFTWARE_CHECK);
  CHECK_EQ(last_trap.tval, SWCHECK_LANDING_PAD);
  csr_clear(mstatus, MSTATUS_MPELP);

  place_stub(TARGET, pad_1_5, pad_1_5_end);
  CHECK_EQ(run_u(jalr_a0, 0, TARGET, 0), CAUSE_SOFTWARE_CHECK);
  CHECK_EQ(last_trap.epc, TARGET);
  CHECK_EQ(last_trap.tval, SWCHECK_LANDING_PAD);
  csr_clear(mstatus, MSTATUS_MPELP);

  check_entered(run_u(call_1_5, 0, TARGET, 0), call_1_5, 1);

  csr_clear(CSR_SENVCFG, SENVCFG_LPE);
}

int main(void)
{
  check_match();
  check_mismatch();
  check_pad_fetch();
  check_gate();
  check_no_domcall();
  check_zicfilp();
  return report("t-lpad");
}
