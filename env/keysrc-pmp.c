// KEYSRC=pmp: keys come from dmpcfg, one nibble per PMP entry.
//
// A TOR chain over CVA6's 8 entries, all RWX so that only the domain check can
// deny an access:
//   entry 0      [0, page 1)                 code, data, page 0
//   entries 1-6  page k
//   entry 7      page 7 and the two DRS pages
// Anything above the DRS (the stacks) matches no entry, so S and U mode
// cannot reach it.

#include "env.h"

#define PMP_R 0x01
#define PMP_W 0x02
#define PMP_X 0x04
#define PMP_TOR 0x08
#define PMP_RWX_TOR (PMP_R | PMP_W | PMP_X | PMP_TOR)

extern char drs_top[];

static uint64_t dmpcfg;

void keysrc_reset(void)
{
  csr_write(pmpaddr0, PAGE(1) >> 2);
  csr_write(pmpaddr1, PAGE(2) >> 2);
  csr_write(pmpaddr2, PAGE(3) >> 2);
  csr_write(pmpaddr3, PAGE(4) >> 2);
  csr_write(pmpaddr4, PAGE(5) >> 2);
  csr_write(pmpaddr5, PAGE(6) >> 2);
  csr_write(pmpaddr6, PAGE(7) >> 2);
  csr_write(pmpaddr7, (uintptr_t)drs_top >> 2);

  uint64_t cfg = 0;
  for (int i = 0; i < NPMP; i++)
    cfg |= (uint64_t)PMP_RWX_TOR << (8 * i);
  csr_write(pmpcfg0, cfg);

  dmpcfg = 0;
  for (int k = 0; k < NKEYS; k++)
    key_set(k, k);
}

// Page p is PMP entry p. Entry 0 also holds the code and data, and entry 7 the
// DRS, so retagging page 0 or 7 retags those too.
void key_set(int page, int key)
{
  dmpcfg &= ~(0xfULL << (4 * page));
  dmpcfg |= (uint64_t)key << (4 * page);
  csr_write(CSR_DMPCFG0, dmpcfg);
}
