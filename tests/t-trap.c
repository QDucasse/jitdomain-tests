// T-TRAP: traps save {curdom, dpend} in xPD/xPPEND and enter DOM0; xret
// restores both. Covers ecall, ebreak and a fault inside DOM1, resuming DOM1
// after a fault, mret and sret restoring a pending entry, an interrupt taken
// while an entry is pending, and traps into S mode, domain faults included.
//
// Callers and the S-mode handler (t-trap.S) run from the image, key 0. DOM1's
// code is copied into page 1, key 1. Interrupts are supervisor software
// interrupts raised through mip.SSIP, so no platform timer is needed.

#include "env.h"

STUB(call_1_5); STUB(pad_ecall); STUB(pad_ebreak); STUB(pad_load); STUB(nopad);
STUB(s_handler); STUB(s_sret); STUB(stub_ecall);

#define GATE(from, to) (1ULL << (8 * (from) + (to)))
#define CODE1 (PAGE(1) + 0x100)

#define CAUSE_BREAKPOINT 3
#define CAUSE_SSI ((1ULL << 63) | 1)
#define MIP_SSIP (1ULL << 1)
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

// DOM0 and DOM1 run from key 0, DOM1 also from key 1, and DOM0 may domcall
// DOM1
static void setup(void)
{
  dom_reset();
  rights_set(0, 0, DR_X);
  rights_set(1, 0, DR_X);
  rights_set(1, 1, DR_X);
  rights_apply();
  csr_write(CSR_DGATE, GATE(0, 1));
  csr_write(CSR_DSP, (uintptr_t)drs_top);
}

// A trap out of DOM1 saves DOM1 in MPD, with nothing pending, and the
// monitor runs in DOM0
static void check_from_dom1(uint64_t cause, uint64_t want)
{
  CHECK_EQ(cause, want);
  CHECK_EQ(last_trap.mdstatus, xpd(1));
  CHECK_EQ(csr_read(CSR_CURDOM), 0);
}

static void check_exceptions(void)
{
  setup();
  place_stub(CODE1, pad_ecall, pad_ecall_end);
  check_from_dom1(run_u(call_1_5, 0, CODE1, 0), CAUSE_ECALL_U);
  CHECK_EQ(reg(18), 1);

  setup();
  place_stub(CODE1, pad_ebreak, pad_ebreak_end);
  check_from_dom1(run_u(call_1_5, 0, CODE1, 0), CAUSE_BREAKPOINT);
  CHECK_EQ(last_trap.epc, CODE1 + 8);

  // A domain fault inside DOM1, then mret back into DOM1 at the load once
  // DOM1 may read page 3
  setup();
  place_stub(CODE1, pad_load, pad_load_end);
  *(volatile uint64_t *)PAGE(3) = 0x1234;
  check_from_dom1(run_u(call_1_5, 0, CODE1, PAGE(3)), CAUSE_DOMAIN_ACCESS);
  CHECK_EQ(last_trap.epc, CODE1 + 4);
  CHECK_EQ(last_trap.tval, PAGE(3));

  rights_set(1, 3, DR_R);
  rights_apply();
  CHECK_OK(run_lower((void *)last_trap.epc, PRV_U, last_trap.mdstatus, 0, PAGE(3)), PRV_U);
  CHECK_EQ(last_trap.a0, 0x1234);
  CHECK_EQ(reg(18), 1);
  CHECK_EQ(last_trap.mdstatus, xpd(1));
}

// mret with MPPEND set restores the pending entry: the target must be a
// matching pad, which then enters the pending domain
static void check_mret_pending(void)
{
  setup();
  place_stub(CODE1, pad_ecall, pad_ecall_end);
  CHECK_OK(run_lower((void *)CODE1, PRV_U, xppend(1, 5), 0, 0), PRV_U);
  CHECK_EQ(reg(18), 1);
  CHECK_EQ(last_trap.mdstatus, xpd(1));

  place_stub(CODE1, nopad, nopad_end);
  CHECK_CFI_FAULT(run_lower((void *)CODE1, PRV_U, xppend(1, 5), 0, 0), DCAUSE_LPAD);
  CHECK_EQ(last_trap.epc, CODE1);
  CHECK_EQ(last_trap.mdstatus, xppend(1, 5));

  // The same from S mode
  place_stub(CODE1, pad_ecall, pad_ecall_end);
  CHECK_OK(run_lower((void *)CODE1, PRV_S, xppend(1, 5), 0, 0), PRV_S);
  CHECK_EQ(reg(18), 1);
}

// An interrupt while an entry is pending is taken at the pad: MPPEND holds
// the entry, and mret back to the pad completes it
static void check_interrupt_pending(void)
{
  setup();
  place_stub(CODE1, pad_ecall, pad_ecall_end);
  csr_set(mie, MIP_SSIP);
  csr_set(mip, MIP_SSIP);
  CHECK_EQ(run_lower((void *)CODE1, PRV_U, xppend(1, 5), 0, 0), CAUSE_SSI);
  CHECK_EQ(last_trap.epc, CODE1);
  CHECK_EQ(last_trap.mdstatus, xppend(1, 5));
  CHECK_EQ(csr_read(CSR_CURDOM), 0);

  csr_clear(mip, MIP_SSIP);
  csr_clear(mie, MIP_SSIP);
  CHECK_OK(run_lower((void *)last_trap.epc, PRV_U, last_trap.mdstatus, 0, 0), PRV_U);
  CHECK_EQ(reg(18), 1);
}

// s_sret runs in S mode but returns into U pages, so it runs from the S alias
// of the image with the U-mode tables
static uint64_t run_sret(uintptr_t target, uint64_t dstatus)
{
  uint64_t saved = keysrc_satp[PRV_S];
  keysrc_satp[PRV_S] = keysrc_satp[PRV_U];
  uint64_t cause = run_s(keysrc_s_alias(s_sret), 0, target, dstatus);
  keysrc_satp[PRV_S] = saved;
  return cause;
}

// Traps delegated to S mode save the domain in dstatus.SPD/SPPEND and enter
// DOM0; sret restores them
static void check_supervisor(void)
{
  setup();
  csr_write(stvec, (uintptr_t)keysrc_s_alias(s_handler));

  // An ecall from U mode in DOM1
  csr_write(medeleg, 1 << CAUSE_ECALL_U);
  CHECK_OK(run_u(stub_ecall, 1, 0, 0), PRV_S);
  CHECK_EQ(reg(23), CAUSE_ECALL_U);   // s7, scause
  CHECK_EQ(reg(20), DSTATUS_BASE | xpd(1));   // s4, dstatus
  CHECK_EQ(reg(21), 0);   // s5, curdom in the handler
  CHECK_EQ(last_trap.mdstatus, 0);
  csr_write(medeleg, 0);

  // Domain faults are delegable. A domain access fault in DOM1, with the
  // address in stval
  place_stub(CODE1, pad_load, pad_load_end);
  csr_write(medeleg, 1 << CAUSE_DOMAIN_ACCESS);
  CHECK_OK(run_u(call_1_5, 0, CODE1, PAGE(3)), PRV_S);
  CHECK_EQ(reg(23), CAUSE_DOMAIN_ACCESS);
  CHECK_EQ(reg(24), PAGE(3));   // s8, stval
  CHECK_EQ(reg(20), DSTATUS_BASE | xpd(1));
  CHECK_EQ(reg(21), 0);
  csr_write(medeleg, 0);

  // A domain CFI fault at a non-pad, with its pc in stval and the subcode in
  // dcause
  place_stub(CODE1, nopad, nopad_end);
  csr_write(medeleg, 1 << CAUSE_DOMAIN_CFI);
  CHECK_OK(run_lower((void *)CODE1, PRV_U, xppend(1, 5), 0, 0), PRV_S);
  CHECK_EQ(reg(23), CAUSE_DOMAIN_CFI);
  CHECK_EQ(reg(24), CODE1);
  CHECK_EQ(reg(25), DCAUSE_LPAD);   // s9, dcause
  CHECK_EQ(reg(20), DSTATUS_BASE | xppend(1, 5));
  CHECK_EQ(reg(21), 0);
  csr_write(medeleg, 0);

  // An interrupt at a pad with an entry pending
  place_stub(CODE1, pad_ecall, pad_ecall_end);
  csr_write(mideleg, MIP_SSIP);
  csr_set(mie, MIP_SSIP);
  csr_set(mip, MIP_SSIP);
  CHECK_OK(run_lower((void *)CODE1, PRV_U, xppend(1, 5), 0, 0), PRV_S);
  CHECK_EQ(reg(23), CAUSE_SSI);
  CHECK_EQ(reg(20), DSTATUS_BASE | xppend(1, 5));
  CHECK_EQ(reg(21), 0);
  csr_clear(mip, MIP_SSIP);
  csr_clear(mie, MIP_SSIP);
  csr_write(mideleg, 0);

  // sret with SPPEND set enters the pad's domain; without a pad it faults
  CHECK_OK(run_sret(CODE1, DSTATUS_BASE | xppend(1, 5)), PRV_U);
  CHECK_EQ(reg(18), 1);
  CHECK_EQ(last_trap.mdstatus, xpd(1));

  place_stub(CODE1, nopad, nopad_end);
  CHECK_CFI_FAULT(run_sret(CODE1, DSTATUS_BASE | xppend(1, 5)), DCAUSE_LPAD);
  CHECK_EQ(last_trap.mdstatus, xppend(1, 5));

  // sret restores SPD without a pending entry
  rights_set(4, 1, DR_X);
  rights_apply();
  CHECK_OK(run_sret(CODE1, DSTATUS_BASE | xpd(4)), PRV_U);
  CHECK_EQ(reg(18), 4);
  CHECK_EQ(last_trap.mdstatus, xpd(4));
}

int main(void)
{
  check_exceptions();
  check_mret_pending();
  check_interrupt_pending();
  check_supervisor();
  return report("t-trap");
}
