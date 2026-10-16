// T-ACC: the access check for every (e, k, rights) combination and every
// access kind: load (R), store (W), AMO and LR (R|W), and fetch (X). Then which
// privilege levels are checked: U always, S with dstatus.SCHK, M only through
// locked PMP entries.

#include "env.h"

STUB(stub_ecall); STUB(stub_ld); STUB(stub_sd); STUB(stub_amoadd_d); STUB(stub_lr_d);
STUB(stub_lr_sc_d);

enum { ACC_LOAD, ACC_STORE, ACC_AMO, ACC_LR, ACC_FETCH, NACC };

static const struct {
  const char *name;
  char *start, *end;
  int need;
} accs[NACC] = {
  [ACC_LOAD]  = { "load",  stub_ld,       stub_ld_end,       DR_R },
  [ACC_STORE] = { "store", stub_sd,       stub_sd_end,       DR_W },
  [ACC_AMO]   = { "amo",   stub_amoadd_d, stub_amoadd_d_end, DR_R | DR_W },
  [ACC_LR]    = { "lr",    stub_lr_d,     stub_lr_d_end,     DR_R | DR_W },
  [ACC_FETCH] = { "fetch", stub_ecall,    stub_ecall_end,    DR_X },
};

// Every page holds its data word at offset 0 and a copy of each stub at
// STUB_OFF + 16 * acc, so the code can be fetched with any key
#define STUB_OFF 0x800
#define DATA 0x1122334455667788ULL
#define STORED 0x0000000100000001ULL

static uintptr_t stub_at(int k, int acc)
{
  return PAGE(k) + STUB_OFF + 16 * acc;
}

static volatile uint64_t *data(int k)
{
  return (volatile uint64_t *)PAGE(k);
}

static void place_stubs(void)
{
  for (int k = 0; k < NKEYS; k++)
    for (int a = 0; a < NACC; a++)
      place_stub(stub_at(k, a), accs[a].start, accs[a].end);
}

// One access of kind acc by domain e to page k, with rights r on (e, k).
// The DRS key and the key of the page the stub runs from (c) are chosen
// away from k; e gets X on c and nothing else.
static void one(int e, int k, int r, int acc)
{
  int drskey = k == 7 ? 6 : 7;
  int c = k == 0 ? 1 : 0;
  csr_write(CSR_DSTATUS, DSTATUS_EN | (drskey << DSTATUS_DRSKEY_SHIFT));
  rights_clear();
  rights_set(e, k, r);
  if (acc != ACC_FETCH)
    rights_set(e, c, DR_X);
  rights_apply();
  *data(k) = DATA;

  uintptr_t entry = acc == ACC_FETCH ? stub_at(k, acc) : stub_at(c, acc);
  uint64_t cause = run_u((void *)entry, e, PAGE(k), STORED);
  bool allowed = (r & accs[acc].need) == accs[acc].need;

  if (!allowed) {
    CHECK_DOM_FAULT(cause, acc == ACC_FETCH ? entry : PAGE(k));
    CHECK_EQ(last_trap.epc, entry);
    CHECK_EQ(*data(k), DATA);
  } else {
    CHECK_OK(cause, PRV_U);
    switch (acc) {
    case ACC_LOAD:
    case ACC_LR:
      CHECK_EQ(last_trap.a0, DATA);
      CHECK_EQ(*data(k), DATA);
      break;
    case ACC_STORE:
      CHECK_EQ(*data(k), STORED);
      break;
    case ACC_AMO:
      CHECK_EQ(last_trap.a0, DATA);
      CHECK_EQ(*data(k), DATA + STORED);
      break;
    case ACC_FETCH:
      CHECK_EQ(last_trap.epc, entry);
      break;
    }
  }
}

// QUICK=1 (Verilator) samples three rights values per cell: none, exactly
// the needed right, and everything but the needed right
#if QUICK
#define NRIGHTS 3
static int rights_sample(int acc, int i)
{
  int need = accs[acc].need;
  return i == 0 ? 0 : i == 1 ? need : 7 & ~need;
}
#else
#define NRIGHTS 8
static int rights_sample(int acc, int i)
{
  (void)acc;
  return i;
}
#endif

static void check_matrix(void)
{
  dom_reset();
  place_stubs();
  for (int e = 0; e < NDOMS; e++)
    for (int k = 0; k < NKEYS; k++)
      for (int acc = 0; acc < NACC; acc++)
        for (int i = 0; i < NRIGHTS; i++) {
          int r = rights_sample(acc, i);
          int before = failures;
          one(e, k, r, acc);
          if (failures != before && failures <= 50)
            printf("    (e=%d k=%d r=%d %s)\n", e, k, r, accs[acc].name);
        }
}

// An entry of the matrix only affects its own (e, k): with every other entry
// granted, clearing one still denies exactly that access
static void check_isolation(void)
{
  dom_reset();
  place_stubs();
  csr_write(CSR_DSTATUS, DSTATUS_EN | (7 << DSTATUS_DRSKEY_SHIFT));
  for (int e = 0; e < NDOMS; e++) {
    for (int k = 1; k < 6; k++) {  // k - 1 and k + 1 stay clear of the DRS key (7)
      for (int ee = 0; ee < NDOMS; ee++)
        for (int kk = 0; kk < NKEYS; kk++)
          rights_set(ee, kk, DR_R | DR_W | DR_X);
      rights_set(e, k, DR_W | DR_X);
      rights_apply();
      CHECK_DOM_FAULT(run_u(stub_ld, e, PAGE(k), 0), PAGE(k));
      CHECK_OK(run_u(stub_ld, e, PAGE(k - 1), 0), PRV_U);
      CHECK_OK(run_u(stub_ld, e, PAGE(k + 1), 0), PRV_U);
      CHECK_OK(run_u(stub_ld, (e + 1) % NDOMS, PAGE(k), 0), PRV_U);
    }
  }
}

// A successful LR/SC pair needs R|W
static void check_lr_sc(void)
{
  dom_reset();
  rights_set(1, 0, DR_X);
  rights_set(1, 3, DR_R | DR_W);
  rights_apply();
  *data(3) = DATA;
  CHECK_OK(run_u(stub_lr_sc_d, 1, PAGE(3), STORED), PRV_U);
  CHECK_EQ(last_trap.a0, 0);
  CHECK_EQ(*data(3), STORED);
}

// dstatus.EN = 0 disables the check, the DRS key included
static void check_disabled(void)
{
  dom_reset();
  place_stubs();
  dom_disable();
  for (int k = 0; k < NKEYS; k++) {
    CHECK_OK(run_u((void *)stub_at(k, ACC_LOAD), 3, PAGE(k), 0), PRV_U);
    CHECK_OK(run_u((void *)stub_at(k, ACC_STORE), 3, PAGE(k), STORED), PRV_U);
  }
}

// S mode is checked only when dstatus.SCHK = 1
static void check_supervisor(void)
{
  dom_reset();
  place_stubs();
  CHECK_OK(run_s((void *)stub_at(2, ACC_LOAD), 1, PAGE(3), 0), PRV_S);
  CHECK_OK(run_s((void *)stub_at(2, ACC_STORE), 1, PAGE(3), STORED), PRV_S);

  csr_set(CSR_DSTATUS, DSTATUS_SCHK);
  CHECK_DOM_FAULT(run_s((void *)stub_at(2, ACC_LOAD), 1, PAGE(3), 0), stub_at(2, ACC_LOAD));
  rights_set(1, 2, DR_X);
  rights_apply();
  CHECK_DOM_FAULT(run_s((void *)stub_at(2, ACC_LOAD), 1, PAGE(3), 0), PAGE(3));
  rights_set(1, 3, DR_R);
  rights_apply();
  CHECK_OK(run_s((void *)stub_at(2, ACC_LOAD), 1, PAGE(3), 0), PRV_S);
  CHECK_DOM_FAULT(run_s((void *)stub_at(2, ACC_STORE), 1, PAGE(3), STORED), PAGE(3));
}

#if KEYSRC_PMP
// M mode is checked only for accesses that match a locked PMP entry. Locks
// entry 3 until reset, so this runs last. M mode does not translate, so its
// keys always come from PMP: KEYSRC=pmp only.
static void check_machine(void)
{
  dom_reset();
  *data(3) = DATA;
  *data(4) = DATA;

  uint64_t before = m_traps;
  CHECK_EQ(*data(4), DATA);
  CHECK_EQ(*data(3), DATA);
  CHECK_EQ(m_traps, before);

  csr_set(pmpcfg0, 0x80ULL << 24);
  before = m_traps;
  (void)*data(3);
  CHECK_EQ(m_traps, before + 1);
  CHECK_EQ(last_trap.cause, CAUSE_DOMAIN_ACCESS);
  CHECK_EQ(last_trap.tval, PAGE(3));
  (void)*data(4);
  CHECK_EQ(m_traps, before + 1);

  // The monitor skips a faulting compressed instruction by its 2 bytes
  register uintptr_t p __asm__("a0") = PAGE(3);
  uint64_t reached = 0;
  __asm__ volatile("c.ld a1, 0(%1)\n\tli %0, 1" : "+r"(reached) : "r"(p) : "a1", "memory");
  CHECK_EQ(m_traps, before + 2);
  CHECK_EQ(last_trap.tval, PAGE(3));
  CHECK_EQ(reached, 1);

  rights_set(0, 3, DR_R);
  rights_apply();
  CHECK_EQ(*data(3), DATA);
  *data(3) = 0;
  CHECK_EQ(m_traps, before + 3);
  CHECK_EQ(last_trap.cause, CAUSE_DOMAIN_ACCESS);
  CHECK_EQ(*data(3), DATA);
}
#endif

int main(void)
{
  check_matrix();
  check_isolation();
  check_lr_sc();
  check_disabled();
  check_supervisor();
#if KEYSRC_PMP
  check_machine();
#endif
  return report("t-acc");
}
