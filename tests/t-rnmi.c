// T-RNMI: an RNMI saves {curdom, dpend} in mndstatus.MNPD/MNPPEND and enters
// DOM0; mnret restores both and clears MNPPEND. An exception inside the RNMI
// handler is an ordinary M-mode trap: it saves into mdstatus and leaves
// mndstatus alone.
//
// Everything runs in M mode, where accesses are not domain-checked. The mnret
// checks need Smrnmi only. The RNMI itself is raised through an M-mode double
// trap, so those checks also need Smdbltrp.

#include "env.h"

#define CSR_MNSTATUS 0x744
#define MSTATUS_MDT (1ULL << 42)
#define CAUSE_ECALL_M 11

typedef struct {
  uint64_t xdstatus, curdom;
} seen_t;

seen_t m_mnret_pad(uint64_t mndstatus);
void m_rnmi_pending(uint64_t mndstatus);
extern char m_rnmi_nopad[];

// Filled in by the RNMI handlers in t-rnmi.S
extern volatile struct {
  uint64_t mndstatus, curdom, mnepc;      // on RNMI entry
  uint64_t exc_mdstatus, exc_mndstatus;   // in the RNMI exception handler
  uint64_t exc_mcause;
  uint64_t mndstatus_after;               // after the RNMI exception
} rnmi_seen;

static uint64_t xpd(int dom)
{
  return (uint64_t)dom << DSTATUS_XPD_SHIFT;
}

static uint64_t xppend(int dom, int label)
{
  return (uint64_t)PEND(dom, label) << DSTATUS_XPPEND_SHIFT;
}

// A CSR access the monitor had to skip means the CSR does not exist
static bool has_smrnmi(void)
{
  uint64_t before = m_traps;
  (void)csr_read(CSR_MNSTATUS);
  return m_traps == before;
}

static bool has_smdbltrp(void)
{
  csr_set(mstatus, MSTATUS_MDT);
  bool mdt = csr_read(mstatus) & MSTATUS_MDT;
  csr_clear(mstatus, MSTATUS_MDT);
  return mdt;
}

// mndstatus holds MNPD and MNPPEND only
static void check_csr(void)
{
  uint64_t before = m_traps;
  csr_write(CSR_MNDSTATUS, -1);
  CHECK_EQ(csr_read(CSR_MNDSTATUS), MDSTATUS_MASK);
  csr_write(CSR_MNDSTATUS, 0);
  CHECK_EQ(csr_read(CSR_MNDSTATUS), 0);
  CHECK_EQ(m_traps, before);
}

// mnret restores curdom and dpend, then clears MNPPEND and keeps MNPD
static void check_mnret(void)
{
  dom_reset();
  seen_t seen = m_mnret_pad(xppend(1, 5));
  CHECK_EQ(seen.xdstatus, 0);
  CHECK_EQ(seen.curdom, 1);
  seen = m_mnret_pad(xpd(3));
  CHECK_EQ(seen.xdstatus, xpd(3));
  CHECK_EQ(seen.curdom, 3);
  CHECK_EQ(csr_read(CSR_CURDOM), 0);
}

// An RNMI taken while an entry is pending, in DOM2: the handler runs in DOM0
// with nothing pending, and mndstatus holds both. The ecall in the handler
// saves into mdstatus and leaves mndstatus alone.
static void check_rnmi_entry(void)
{
  dom_reset();
  uint64_t state = xpd(2) | xppend(1, 5);
  m_rnmi_pending(state);
  CHECK_EQ(rnmi_seen.mndstatus, state);
  CHECK_EQ(rnmi_seen.curdom, 0);
  CHECK_EQ(rnmi_seen.mnepc, (uintptr_t)m_rnmi_nopad);
  CHECK_EQ(rnmi_seen.exc_mcause, CAUSE_ECALL_M);
  CHECK_EQ(rnmi_seen.exc_mdstatus, 0);
  CHECK_EQ(rnmi_seen.exc_mndstatus, state);
  CHECK_EQ(rnmi_seen.mndstatus_after, state);
  CHECK_EQ(csr_read(CSR_CURDOM), 0);
}

int main(void)
{
  if (!has_smrnmi()) {
    printf("t-rnmi: no Smrnmi, skipped\n");
    return report("t-rnmi");
  }
  check_csr();
  check_mnret();
  if (has_smdbltrp())
    check_rnmi_entry();
  else
    printf("t-rnmi: no Smdbltrp, RNMI entry not tested\n");
  return report("t-rnmi");
}
