// T-DRS: the Domain Return Stack. A domcall/domret round trip, domret to the
// recorded pc and domain, nesting 64 deep, a corrupted dsp or record, a DRS
// that runs off its pages, dstatus.DRSKEY, and ordinary and typed accesses to
// DRS pages.
//
// Callers (t-drs.S) run in DOM0 from the image, key 0; DOM1's code is copied
// into page 1, key 1. The DRS pages have key 7, and DOM0 and DOM1 have no
// rights on it: implicit DRS accesses need none.

#include "env.h"

STUB(call_1_5); STUB(call_nest); STUB(ret_only);
STUB(callee); STUB(nest); STUB(land); STUB(land_at2);
STUB(stub_ld); STUB(stub_sd); STUB(stub_dsd);

#define GATE(from, to) (1ULL << (8 * (from) + (to)))
#define CODE1 (PAGE(1) + 0x100)
#define TOP ((uintptr_t)drs_top)

static uint64_t reg(int i)
{
  return last_trap.x[i];
}

// Record i from the top of the DRS: {pc, domain}
static volatile uint64_t *record(int i)
{
  return (volatile uint64_t *)(TOP - 16 * (i + 1));
}

// DOM0 runs from key 0 and DOM1 from keys 0 and 1, and DOM0 may domcall DOM1;
// the DRS is empty
static void setup(void)
{
  dom_reset();
  rights_set(0, 0, DR_X);
  rights_set(1, 0, DR_X);
  rights_set(1, 1, DR_X);
  rights_apply();
  csr_write(CSR_DGATE, GATE(0, 1));
  csr_write(CSR_DSP, TOP);
}

// A DRS fault: the CFI fault on the instruction at pc, with dsp unchanged
// and nothing pending
static void check_drs_fault(uint64_t cause, uintptr_t pc, uint64_t dsp)
{
  CHECK_CFI_FAULT(cause, DCAUSE_DRS);
  CHECK_EQ(last_trap.epc, pc);
  CHECK_EQ(last_trap.tval, pc);
  CHECK_EQ(last_trap.mdstatus & DSTATUS_XPPEND, 0);
  CHECK_EQ(csr_read(CSR_DSP), dsp);
}

// A push or pop at addr, where no memory is mapped. Without translation that
// is an access fault, which becomes a DRS fault; with translation it stays a
// page fault on addr, so that the OS can grow the DRS on demand.
static void check_drs_unmapped(uint64_t cause, uintptr_t pc, uint64_t dsp, uintptr_t addr, bool push)
{
#if KEYSRC_SV39
  CHECK_EQ(cause, push ? CAUSE_STORE_PAGE_FAULT : CAUSE_LOAD_PAGE_FAULT);
  CHECK_EQ(last_trap.epc, pc);
  CHECK_EQ(last_trap.tval, addr);
  CHECK_EQ(last_trap.mdstatus & DSTATUS_XPPEND, 0);
  CHECK_EQ(csr_read(CSR_DSP), dsp);
#else
  (void)addr;
  (void)push;
  check_drs_fault(cause, pc, dsp);
#endif
}

// DOM0 calls DOM1, which returns: back in DOM0 after the domcall, with the
// DRS empty again and the record left in memory
static void check_round_trip(void)
{
  setup();
  place_stub(CODE1, callee, callee_end);
  CHECK_OK(run_u(call_1_5, 0, CODE1, 0), PRV_U);
  CHECK_EQ(reg(19), 1);   // s3, in the callee
  CHECK_EQ(reg(18), 0);   // s2, back in the caller
  CHECK_EQ(last_trap.mdstatus, 0);
  CHECK_EQ(last_trap.epc, (uintptr_t)call_1_5 + 8);
  CHECK_EQ(csr_read(CSR_DSP), TOP);
  CHECK_EQ(record(0)[0], (uintptr_t)call_1_5 + 4);
  CHECK_EQ(record(0)[1], 0);
}

// domret goes to the recorded pc, 2-byte aligned or not, in the recorded
// domain, whatever the domain it runs in
static void check_return_target(void)
{
  for (int at2 = 0; at2 < 2; at2++) {
    for (int d = 0; d < NDOMS; d++) {
      setup();
      rights_set(d, 1, DR_X);
      rights_apply();
      if (at2)
        place_stub(CODE1, land_at2, land_at2_end);
      else
        place_stub(CODE1, land, land_end);
      record(0)[0] = CODE1 + 2 * at2;
      record(0)[1] = d;
      csr_write(CSR_DSP, TOP - 16);

      int before = failures;
      CHECK_OK(run_u(ret_only, 1, 0, 0), PRV_U);
      CHECK_EQ(reg(18), d);
      CHECK_EQ(last_trap.mdstatus, (uint64_t)d << DSTATUS_XPD_SHIFT);
      CHECK_EQ(csr_read(CSR_DSP), TOP);
      if (failures != before)
        printf("    (domret to DOM%d, %s)\n", d, at2 ? "2-byte aligned" : "4-byte aligned");
    }
  }
}

// Nested domcalls from DOM1 into DOM1, 64 deep, then all the domrets
static void check_nesting(void)
{
  const int depth = 64;
  setup();
  csr_write(CSR_DGATE, GATE(0, 1) | GATE(1, 1));
  place_stub(CODE1, nest, nest_end);
  CHECK_OK(run_u(call_nest, 0, CODE1, depth), PRV_U);
  CHECK_EQ(reg(22), depth);   // s6, entries
  CHECK_EQ(reg(18), 0);
  CHECK_EQ(csr_read(CSR_DSP), TOP);

  // The outer record returns to the caller, the others into nest, after its
  // domcall
  CHECK_EQ(record(0)[0], (uintptr_t)call_nest + 8);
  CHECK_EQ(record(0)[1], 0);
  for (int i = 1; i < depth; i++) {
    CHECK_EQ(record(i)[0], CODE1 + 20);
    CHECK_EQ(record(i)[1], 1);
  }
}

// A misaligned dsp, or one that points into a page without the DRS key,
// faults on push and on pop, before anything changes
static void check_bad_dsp(void)
{
  static const struct {
    const char *name;
    uintptr_t dsp;
  } cases[] = {
    { "misaligned",   TOP - 8 },
    { "a key-6 page", PAGE(6) + 0x800 },
  };

  for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    int before = failures;
    setup();
    place_stub(CODE1, callee, callee_end);
    csr_write(CSR_DSP, cases[i].dsp);
    check_drs_fault(run_u(call_1_5, 0, CODE1, 0), (uintptr_t)call_1_5, cases[i].dsp);
    CHECK_EQ(last_trap.x[1], 0);   // no link

    setup();
    csr_write(CSR_DSP, cases[i].dsp);
    check_drs_fault(run_u(ret_only, 1, 0, 0), (uintptr_t)ret_only, cases[i].dsp);
    CHECK_EQ(last_trap.mdstatus, 1 << DSTATUS_XPD_SHIFT);   // still DOM1
    if (failures != before)
      printf("    (dsp %s)\n", cases[i].name);
  }

  // dsp = 0 pushes at the top of the address space and pops at 0, and popping
  // an empty DRS reads past its top: no memory there
  setup();
  place_stub(CODE1, callee, callee_end);
  csr_write(CSR_DSP, 0);
  check_drs_unmapped(run_u(call_1_5, 0, CODE1, 0), (uintptr_t)call_1_5, 0, -16, true);
  setup();
  csr_write(CSR_DSP, 0);
  check_drs_unmapped(run_u(ret_only, 1, 0, 0), (uintptr_t)ret_only, 0, 0, false);
  setup();
  check_drs_unmapped(run_u(ret_only, 1, 0, 0), (uintptr_t)ret_only, TOP, TOP, false);
}

// A record whose domain is out of range faults in domret
static void check_bad_record(void)
{
  setup();
  place_stub(CODE1, land, land_end);
  record(0)[0] = CODE1;
  record(0)[1] = NDOMS;
  csr_write(CSR_DSP, TOP - 16);
  check_drs_fault(run_u(ret_only, 1, 0, 0), (uintptr_t)ret_only, TOP - 16);
}

// The DRS key is dstatus.DRSKEY: with DRSKEY = 6 page 6 can hold the DRS,
// and the key-7 pages can no longer
static void check_drskey(void)
{
  setup();
  csr_write(CSR_DSTATUS, DSTATUS_EN | (6 << DSTATUS_DRSKEY_SHIFT));
  place_stub(CODE1, callee, callee_end);

  csr_write(CSR_DSP, PAGE(6) + PAGE_SIZE);
  CHECK_OK(run_u(call_1_5, 0, CODE1, 0), PRV_U);
  CHECK_EQ(reg(18), 0);
  CHECK_EQ(csr_read(CSR_DSP), PAGE(6) + PAGE_SIZE);

  csr_write(CSR_DSP, TOP);
  check_drs_fault(run_u(call_1_5, 0, CODE1, 0), (uintptr_t)call_1_5, TOP);
}

// No domain reaches the DRS pages from U mode with ordinary or typed accesses,
// whatever its rights on the DRS key; S mode can
static void check_access(void)
{
  setup();
  for (int d = 0; d < NDOMS; d++) {
    rights_set(d, 0, DR_X);
    rights_set(d, 7, DR_R | DR_W | DR_X);
  }
  rights_apply();

  uintptr_t a = (uintptr_t)drs_base;
  for (int d = 0; d < NDOMS; d++) {
    int before = failures;
    CHECK_DOM_FAULT(run_u(stub_ld, d, a, 0), a);
    CHECK_DOM_FAULT(run_u(stub_sd, d, a, 0), a);
    CHECK_DOM_FAULT(run_u(stub_dsd, d, a, 0), a);
    if (failures != before)
      printf("    (DOM%d)\n", d);
  }

  *(volatile uint64_t *)a = 0;
  CHECK_OK(run_s(stub_sd, 1, a, 0x5a), PRV_S);
  CHECK_EQ(*(volatile uint64_t *)a, 0x5a);
  CHECK_OK(run_s(stub_ld, 1, a, 0), PRV_S);
  CHECK_EQ(last_trap.a0, 0x5a);
}

int main(void)
{
  check_round_trip();
  check_return_target();
  check_nesting();
  check_bad_dsp();
  check_bad_record();
  check_drskey();
  check_access();
  return report("t-drs");
}
