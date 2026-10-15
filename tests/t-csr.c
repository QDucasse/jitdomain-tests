// T-CSR: reset values, field masks, and access rules of the JITDomain CSRs.
// curdom and dcause cannot be written at any privilege; only curdom is
// readable from U mode.

#include "env.h"

#define CSR_STUBS(a) STUB(stub_csrr_##a); STUB(stub_csrw_##a)
CSR_STUBS(0x5c0); CSR_STUBS(0x5c1); CSR_STUBS(0x5c2); CSR_STUBS(0x5c3);
CSR_STUBS(0x5c4); CSR_STUBS(0x5c5); CSR_STUBS(0x5c6); CSR_STUBS(0xdc0);
CSR_STUBS(0xcc0); CSR_STUBS(0x7c3); CSR_STUBS(0x7c4); CSR_STUBS(0x7c5);

static const struct {
  uint64_t addr;
  char *rd, *wr;
  int min_rd, min_wr;   // lowest privilege allowed to read / write; -1: never
} csrs[] = {
  { CSR_DSTATUS,  stub_csrr_0x5c0, stub_csrw_0x5c0, PRV_S, PRV_S },
  { CSR_DRIGHTS0, stub_csrr_0x5c1, stub_csrw_0x5c1, PRV_S, PRV_S },
  { CSR_DRIGHTS1, stub_csrr_0x5c2, stub_csrw_0x5c2, PRV_S, PRV_S },
  { CSR_DRIGHTS2, stub_csrr_0x5c3, stub_csrw_0x5c3, PRV_S, PRV_S },
  { CSR_DTARGET,  stub_csrr_0x5c4, stub_csrw_0x5c4, PRV_S, PRV_S },
  { CSR_DGATE,    stub_csrr_0x5c5, stub_csrw_0x5c5, PRV_S, PRV_S },
  { CSR_DSP,      stub_csrr_0x5c6, stub_csrw_0x5c6, PRV_S, PRV_S },
  { CSR_DCAUSE,   stub_csrr_0xdc0, stub_csrw_0xdc0, PRV_S, -1 },
  { CSR_CURDOM,   stub_csrr_0xcc0, stub_csrw_0xcc0, PRV_U, -1 },
  { CSR_DMPCFG0,  stub_csrr_0x7c3, stub_csrw_0x7c3, PRV_M, PRV_M },
  { CSR_MDSTATUS, stub_csrr_0x7c5, stub_csrw_0x7c5, PRV_M, PRV_M },
};
#define NCSRS (sizeof(csrs) / sizeof(csrs[0]))

static void check_reset_values(void)
{
  CHECK_EQ(csr_read(CSR_DSTATUS), 0);
  CHECK_EQ(csr_read(CSR_DRIGHTS0), 0);
  CHECK_EQ(csr_read(CSR_DRIGHTS1), 0);
  CHECK_EQ(csr_read(CSR_DRIGHTS2), 0);
  CHECK_EQ(csr_read(CSR_DTARGET), dtarget_identity());
  CHECK_EQ(csr_read(CSR_DGATE), 0);
  CHECK_EQ(csr_read(CSR_DCAUSE), 0);
  CHECK_EQ(csr_read(CSR_CURDOM), 0);
  CHECK_EQ(csr_read(CSR_MDSTATUS), 0);
  CHECK_EQ(csr_read(CSR_DMPCFG0), 0);
}

static void check_masks(void)
{
  csr_write(CSR_DSTATUS, -1);
  CHECK_EQ(csr_read(CSR_DSTATUS), DSTATUS_MASK);
  csr_write(CSR_DSTATUS, 0);

  csr_write(CSR_MDSTATUS, -1);
  CHECK_EQ(csr_read(CSR_MDSTATUS), MDSTATUS_MASK);
  csr_write(CSR_MDSTATUS, 0);

  // 8 domains x 3 bits
  csr_write(CSR_DTARGET, -1);
  CHECK_EQ(csr_read(CSR_DTARGET), 0xffffff);
  csr_write(CSR_DTARGET, dtarget_identity());

  // 8 x 8 gate bits, 192 rights bits
  csr_write(CSR_DGATE, -1);
  CHECK_EQ(csr_read(CSR_DGATE), -1);
  csr_write(CSR_DGATE, 0);
  for (uint64_t pattern = 0x0123456789abcdefULL, i = 0; i < 2; i++, pattern = ~pattern) {
    csr_write(CSR_DRIGHTS0, pattern);
    csr_write(CSR_DRIGHTS1, ~pattern);
    csr_write(CSR_DRIGHTS2, pattern ^ 0xffff0000ffff0000ULL);
    CHECK_EQ(csr_read(CSR_DRIGHTS0), pattern);
    CHECK_EQ(csr_read(CSR_DRIGHTS1), ~pattern);
    CHECK_EQ(csr_read(CSR_DRIGHTS2), pattern ^ 0xffff0000ffff0000ULL);
  }
  csr_write(CSR_DRIGHTS0, 0);
  csr_write(CSR_DRIGHTS1, 0);
  csr_write(CSR_DRIGHTS2, 0);

  // dmpcfg: 16 nibbles {L, k}
  csr_write(CSR_DMPCFG0, 0x7777777777777777ULL);
  CHECK_EQ(csr_read(CSR_DMPCFG0), 0x7777777777777777ULL);
  csr_write(CSR_DMPCFG0, 0);

  csr_write(CSR_DSP, 0x80001230);
  CHECK_EQ(csr_read(CSR_DSP), 0x80001230);
  csr_write(CSR_DSP, 0);
}

// curdom and dcause are read-only from M mode too: the write raises an
// illegal-instruction exception and leaves the value unchanged
static void check_readonly_in_m(void)
{
  uint64_t before = m_traps;
  csr_write(CSR_CURDOM, 5);
  CHECK_EQ(m_traps, before + 1);
  CHECK_EQ(last_trap.cause, CAUSE_ILLEGAL_INSN);
  CHECK_EQ(csr_read(CSR_CURDOM), 0);

  before = m_traps;
  csr_write(CSR_DCAUSE, 2);
  CHECK_EQ(m_traps, before + 1);
  CHECK_EQ(last_trap.cause, CAUSE_ILLEGAL_INSN);
  CHECK_EQ(csr_read(CSR_DCAUSE), 0);

  // A read-modify-write with a zero source does not write, so it is legal
  before = m_traps;
  __asm__ volatile("csrrs zero, " CSR_STR(CSR_CURDOM) ", zero");
  CHECK_EQ(m_traps, before);
}

// Every CSR from S and U mode. The stubs run in DOM0 with checks enabled and
// rights to fetch their own code (key 0).
static void check_privilege(void)
{
  dom_reset();
  rights_set(0, 0, DR_X);
  rights_apply();

  for (int prv = PRV_U; prv <= PRV_S; prv++) {
    for (unsigned i = 0; i < NCSRS; i++) {
      uint64_t ok_cause = prv == PRV_U ? CAUSE_ECALL_U : CAUSE_ECALL_S;
      uint64_t cause = run_lower(csrs[i].rd, prv, 0, 0, 0);
      CHECK_EQ(cause, prv >= csrs[i].min_rd ? ok_cause : CAUSE_ILLEGAL_INSN);

      // Keep dstatus as it is; the other CSRs are restored by dom_reset() below
      uint64_t v = csrs[i].addr == CSR_DSTATUS ? csr_read(CSR_DSTATUS) : 0;
      cause = run_lower(csrs[i].wr, prv, 0, 0, v);
      CHECK_EQ(cause, csrs[i].min_wr >= 0 && prv >= csrs[i].min_wr ? ok_cause : CAUSE_ILLEGAL_INSN);
      dom_reset();
      rights_set(0, 0, DR_X);
      rights_apply();
    }
  }
}

// curdom reads the domain set by the trap return, from U and S mode
static void check_curdom_value(void)
{
  dom_reset();
  for (int d = 0; d < NDOMS; d++)
    rights_set(d, 0, DR_X);
  rights_apply();

  for (int d = 0; d < NDOMS; d++) {
    CHECK_OK(run_u(stub_csrr_0xcc0, d, 0, 0), PRV_U);
    CHECK_EQ(last_trap.a0, d);
    // The trap back into M saved the domain in MPD
    CHECK_EQ(last_trap.mdstatus, (uint64_t)d << DSTATUS_XPD_SHIFT);
    CHECK_EQ(csr_read(CSR_CURDOM), 0);
  }
  for (int d = 0; d < NDOMS; d++) {
    CHECK_OK(run_s(stub_csrr_0xcc0, d, 0, 0), PRV_S);
    CHECK_EQ(last_trap.a0, d);
  }
}

// dmpcfg L: a locked nibble ignores writes until reset; the others stay writable.
// Uses entry 14, which no other check relies on. Must run last.
static void check_dmpcfg_lock(void)
{
  dom_reset();
  uint64_t base = csr_read(CSR_DMPCFG0) & ~(0xfULL << 56);
  csr_write(CSR_DMPCFG0, base | ((uint64_t)(DMPCFG_L | 5) << 56));
  CHECK_EQ(csr_read(CSR_DMPCFG0), base | ((uint64_t)(DMPCFG_L | 5) << 56));
  csr_write(CSR_DMPCFG0, (base ^ 0x1) | (2ULL << 56));
  CHECK_EQ(csr_read(CSR_DMPCFG0), (base ^ 0x1) | ((uint64_t)(DMPCFG_L | 5) << 56));
  csr_write(CSR_DMPCFG0, 0);
  CHECK_EQ(csr_read(CSR_DMPCFG0), (uint64_t)(DMPCFG_L | 5) << 56);
}

int main(void)
{
  check_reset_values();
  check_masks();
  check_readonly_in_m();
  check_privilege();
  check_curdom_value();
  check_dmpcfg_lock();
  return report("t-csr");
}
