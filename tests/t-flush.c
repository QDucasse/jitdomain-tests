// T-FLUSH: the instruction right after a domain change is checked against the
// new domain. The change is a domlpad, a domret, an mret or sret, or a trap
// into S mode; the next instruction is an ordinary load or store, a typed
// load, or the fetch of the next page. Each case runs twice, with the rights
// arranged so that only the new domain may access, then only the old one, so a
// pipeline that checks the access with a stale domain fails one of the two.
//
// Callers and the S-mode handlers (t-flush.S) run from the image, key 0.
// DOM0 may access page 4, DOM1 page 3, and dtarget[1] = 6, the only domain
// that may read page 6. Code that must fall through into the next page is
// placed in the last word of page 1, followed by page 2.

#include "env.h"

STUB(call_1_5); STUB(call_ld); STUB(call_sd); STUB(call_dld);
STUB(pad_ld); STUB(pad_sd); STUB(pad_dld); STUB(pad_ret);
STUB(pad_only); STUB(call_only); STUB(land);
STUB(s_ld); STUB(s_sret); STUB(stub_ld); STUB(stub_ecall);

#define GATE(from, to) (1ULL << (8 * (from) + (to)))
#define CODE1 (PAGE(1) + 0x100)
#define CODE5 (PAGE(5) + 0x100)
#define LAST1 (PAGE(2) - 4)
#define TOP ((uintptr_t)drs_top)

#define DATA3 0x3333333333333333ULL
#define DATA4 0x4444444444444444ULL
#define DATA6 0x6666666666666666ULL

#define MSTATUS_SUM (1ULL << 18)
#define DSTATUS_BASE (DSTATUS_EN | (7 << DSTATUS_DRSKEY_SHIFT))

static uint64_t reg(int i)
{
  return last_trap.x[i];
}

static uint64_t xpd(int dom)
{
  return (uint64_t)dom << DSTATUS_XPD_SHIFT;
}

static uint64_t xppend(int dom, int label)
{
  return (uint64_t)PEND(dom, label) << DSTATUS_XPPEND_SHIFT;
}

static volatile uint64_t *word(int page, int off)
{
  return (volatile uint64_t *)(PAGE(page) + off);
}

// DOM0 and DOM1 run from key 0, DOM1 also from key 1, and DOM0 may domcall
// DOM1. DOM0 may access page 4 and DOM1 page 3; typed accesses from DOM1 go
// to DOM6, which may read page 6.
static void setup(void)
{
  dom_reset();
  rights_set(0, 0, DR_X);
  rights_set(1, 0, DR_X);
  rights_set(1, 1, DR_X);
  rights_set(0, 4, DR_R | DR_W);
  rights_set(1, 3, DR_R | DR_W);
  rights_set(6, 6, DR_R);
  rights_apply();
  csr_write(CSR_DTARGET, (dtarget_identity() & ~(7ULL << 3)) | (6ULL << 3));
  csr_write(CSR_DGATE, GATE(0, 1));
  csr_write(CSR_DSP, TOP);
  *word(3, 0) = DATA3;
  *word(4, 0) = DATA4;
  *word(6, 0) = DATA6;
  *word(3, 8) = 0;
  *word(4, 8) = 0;
}

// A domain access fault on addr by the instruction at pc, taken in domain dom
static void check_fault(uint64_t cause, uintptr_t pc, uintptr_t addr, int dom)
{
  CHECK_DOM_FAULT(cause, addr);
  CHECK_EQ(last_trap.epc, pc);
  CHECK_EQ(last_trap.mdstatus, xpd(dom));
}

// domlpad enters DOM1; the access right after it uses DOM1's rights
static void check_domlpad(void)
{
  setup();
  place_stub(CODE1, pad_ld, pad_ld_end);
  CHECK_OK(run_u(call_1_5, 0, CODE1, PAGE(3)), PRV_U);
  CHECK_EQ(reg(12), DATA3);
  CHECK_EQ(reg(18), 1);
  check_fault(run_u(call_1_5, 0, CODE1, PAGE(4)), CODE1 + 4, PAGE(4), 1);

  setup();
  place_stub(CODE1, pad_sd, pad_sd_end);
  CHECK_OK(run_u(call_1_5, 0, CODE1, PAGE(3) + 8), PRV_U);
  CHECK_EQ(*word(3, 8), PAGE(3) + 8);
  check_fault(run_u(call_1_5, 0, CODE1, PAGE(4) + 8), CODE1 + 4, PAGE(4) + 8, 1);
  CHECK_EQ(*word(4, 8), 0);

  // Typed: e = dtarget[1] = 6 after the pad, not dtarget[0] = 0
  setup();
  place_stub(CODE1, pad_dld, pad_dld_end);
  CHECK_OK(run_u(call_1_5, 0, CODE1, PAGE(6)), PRV_U);
  CHECK_EQ(reg(12), DATA6);
  check_fault(run_u(call_1_5, 0, CODE1, PAGE(4)), CODE1 + 4, PAGE(4), 1);
}

// domret returns to DOM0; the access at the return address uses DOM0's rights
static void check_domret(void)
{
  setup();
  place_stub(CODE1, pad_ret, pad_ret_end);
  CHECK_OK(run_u(call_ld, 0, CODE1, PAGE(4)), PRV_U);
  CHECK_EQ(reg(12), DATA4);
  check_fault(run_u(call_ld, 0, CODE1, PAGE(3)), (uintptr_t)call_ld + 4, PAGE(3), 0);

  CHECK_OK(run_u(call_sd, 0, CODE1, PAGE(4) + 8), PRV_U);
  CHECK_EQ(*word(4, 8), PAGE(4) + 8);
  check_fault(run_u(call_sd, 0, CODE1, PAGE(3) + 8), (uintptr_t)call_sd + 4, PAGE(3) + 8, 0);
  CHECK_EQ(*word(3, 8), 0);

  // Typed: e = dtarget[0] = 0 after the return, not dtarget[1] = 6
  CHECK_OK(run_u(call_dld, 0, CODE1, PAGE(4)), PRV_U);
  CHECK_EQ(reg(12), DATA4);
  check_fault(run_u(call_dld, 0, CODE1, PAGE(6)), (uintptr_t)call_dld + 4, PAGE(6), 0);

  CHECK_EQ(csr_read(CSR_DSP), TOP);
}

// The fetch right after a domain change: falling through from the last word
// of page 1 into page 2 after a domlpad or a domret, and the first fetch
// after an mret
static void check_fetch(void)
{
  // domlpad 1 in page 1, then page 2
  setup();
  place_stub(LAST1, pad_only, pad_only_end);
  place_stub(PAGE(2), land, land_end);
  rights_set(1, 2, DR_X);
  rights_apply();
  CHECK_OK(run_u(call_1_5, 0, LAST1, 0), PRV_U);
  CHECK_EQ(reg(18), 1);

  rights_set(1, 2, 0);
  rights_set(0, 2, DR_X);
  rights_apply();
  check_fault(run_u(call_1_5, 0, LAST1, 0), PAGE(2), PAGE(2), 1);

  // DOM0 calls from page 1 and DOM1 returns to page 2
  setup();
  place_stub(LAST1, call_only, call_only_end);
  place_stub(PAGE(2), land, land_end);
  place_stub(CODE5, pad_ret, pad_ret_end);
  rights_set(0, 1, DR_X);
  rights_set(1, 5, DR_X);
  rights_set(0, 2, DR_X);
  rights_apply();
  CHECK_OK(run_u((void *)LAST1, 0, CODE5, 0), PRV_U);
  CHECK_EQ(reg(18), 0);

  rights_set(0, 2, 0);
  rights_set(1, 2, DR_X);
  rights_apply();
  check_fault(run_u((void *)LAST1, 0, CODE5, 0), PAGE(2), PAGE(2), 0);
  CHECK_EQ(csr_read(CSR_DSP), TOP);

  // mret from the monitor in DOM0 into DOM1
  setup();
  place_stub(PAGE(2), land, land_end);
  rights_set(1, 2, DR_X);
  rights_apply();
  CHECK_OK(run_u((void *)PAGE(2), 1, 0, 0), PRV_U);
  CHECK_EQ(reg(18), 1);

  rights_set(1, 2, 0);
  rights_set(0, 2, DR_X);
  rights_apply();
  check_fault(run_u((void *)PAGE(2), 1, 0, 0), PAGE(2), PAGE(2), 1);
}

// mret into DOM1, directly or through a pending entry
static void check_mret(void)
{
  setup();
  CHECK_OK(run_u(stub_ld, 1, PAGE(3), 0), PRV_U);
  CHECK_EQ(last_trap.a0, DATA3);
  check_fault(run_u(stub_ld, 1, PAGE(4), 0), (uintptr_t)stub_ld, PAGE(4), 1);

  place_stub(CODE1, pad_ld, pad_ld_end);
  CHECK_OK(run_lower((void *)CODE1, PRV_U, xppend(1, 5), 0, PAGE(3)), PRV_U);
  CHECK_EQ(reg(12), DATA3);
  check_fault(run_lower((void *)CODE1, PRV_U, xppend(1, 5), 0, PAGE(4)), CODE1 + 4, PAGE(4), 1);
}

// s_sret runs in S mode but returns into U pages, so it runs from the S alias
// of the image with the U-mode tables, and reads its arguments there
static uint64_t run_sret(uintptr_t target, uint64_t dstatus, uint64_t a1)
{
  static volatile uint64_t args[2];
  args[0] = dstatus;
  args[1] = a1;
  uint64_t saved = keysrc_satp[PRV_S];
  keysrc_satp[PRV_S] = keysrc_satp[PRV_U];
  uint64_t cause = run_s(keysrc_s_alias(s_sret), 0, target,
                         (uintptr_t)keysrc_s_alias((void *)args));
  keysrc_satp[PRV_S] = saved;
  return cause;
}

// sret into DOM1, directly or through a pending entry
static void check_sret(void)
{
  setup();
  place_stub(CODE1, pad_ld, pad_ld_end);
  CHECK_OK(run_sret(CODE1, DSTATUS_BASE | xppend(1, 5), PAGE(3)), PRV_U);
  CHECK_EQ(reg(12), DATA3);
  check_fault(run_sret(CODE1, DSTATUS_BASE | xppend(1, 5), PAGE(4)), CODE1 + 4, PAGE(4), 1);

  // Skip the pad: from CODE1 + 4, DOM1 is restored from SPD alone
  CHECK_OK(run_sret(CODE1 + 4, DSTATUS_BASE | xpd(1), PAGE(3)), PRV_U);
  CHECK_EQ(reg(12), DATA3);
  check_fault(run_sret(CODE1 + 4, DSTATUS_BASE | xpd(1), PAGE(4)), CODE1 + 4, PAGE(4), 1);
}

// A trap from DOM1 into S mode enters DOM0; with SCHK the handler's first
// instruction uses DOM0's rights. The handler reads U pages, hence SUM.
static void check_trap_entry(void)
{
  setup();
  uintptr_t handler = (uintptr_t)keysrc_s_alias(s_ld);
  csr_write(stvec, handler);
  csr_write(medeleg, 1 << CAUSE_ECALL_U);
  csr_set(CSR_DSTATUS, DSTATUS_SCHK);
  csr_set(mstatus, MSTATUS_SUM);

  CHECK_OK(run_u(stub_ecall, 1, 0, PAGE(4)), PRV_S);
  CHECK_EQ(reg(12), DATA4);
  CHECK_EQ(reg(21), 0);   // s5, curdom in the handler
  check_fault(run_u(stub_ecall, 1, 0, PAGE(3)), handler, PAGE(3), 0);

  csr_clear(mstatus, MSTATUS_SUM);
  csr_write(medeleg, 0);
}

int main(void)
{
  check_domlpad();
  check_domret();
  check_fetch();
  check_mret();
  check_sret();
  check_trap_entry();
  return report("t-flush");
}
