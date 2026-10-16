// T-PTE: keys from Sv39 PTEs, built with KEYSRC=sv39 only. The key comes from
// the PTE that maps the access; a key on a non-leaf PTE, or any key with
// dstatus.EN = 0, is a page fault of the access type; the base PTE checks
// come before the domain check; dmpcfg is ignored under translation; a
// superpage leaf gives its whole range one key.
//
// The checks edit an alias of the test pages in the U tree, so the identity
// map the code runs from is never touched:
//   root[1] -> l1    l1[0] -> l0    l0[k] = pages[k], key k
//                    l1[1] = a 2 MiB leaf over the image's region, key 0
// run_lower() flushes the TLB before every stub, so edits take effect at the
// next run.

#include "env.h"
#include "sv39.h"

STUB(stub_ecall); STUB(stub_ld); STUB(stub_sd); STUB(stub_amoadd_d);

#define ALIAS 0x40000000UL
#define APAGE(k) (ALIAS + (uintptr_t)(k) * PAGE_SIZE)
#define MEGA (ALIAS + (1UL << 21))
#define MEGA_MASK ((1UL << 21) - 1)
#define LEAF (PTE_V | PTE_R | PTE_W | PTE_X | PTE_U | PTE_A | PTE_D)
#define DATA 0x1122334455667788ULL
#define STUB_OFF 0x800

static uint64_t l1[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t l0[512] __attribute__((aligned(PAGE_SIZE)));

// The superpage alias of a physical address in the image's region
static uintptr_t mega(uintptr_t pa)
{
  return MEGA + (pa & MEGA_MASK);
}

// dom_reset(), the alias, a distinct word at the start of every page, and X on
// key 0 for DOM1 so that U stubs can run from the image
static void setup(void)
{
  dom_reset();
  memset(l1, 0, sizeof(l1));
  memset(l0, 0, sizeof(l0));
  sv39_root(0)[(ALIAS >> 30) & 511] = pte((uintptr_t)l1, PTE_V);
  l1[0] = pte((uintptr_t)l0, PTE_V);
  l1[1] = pte(PAGE(0) & ~MEGA_MASK, LEAF);
  for (int k = 0; k < NKEYS; k++) {
    l0[k] = pte_key(pte(PAGE(k), LEAF), k);
    *(volatile uint64_t *)PAGE(k) = DATA + k;
  }
  rights_set(1, 0, DR_X);
  rights_apply();
}

// The key belongs to the mapping, not to the physical page
static void check_alias(void)
{
  setup();
  rights_set(1, 2, DR_R);
  rights_apply();
  CHECK_OK(run_u(stub_ld, 1, APAGE(2), 0), PRV_U);
  CHECK_EQ(last_trap.a0, DATA + 2);
  CHECK_DOM_FAULT(run_u(stub_ld, 1, APAGE(3), 0), APAGE(3));

  l0[2] = pte_key(l0[2], 3);
  CHECK_DOM_FAULT(run_u(stub_ld, 1, APAGE(2), 0), APAGE(2));
  CHECK_OK(run_u(stub_ld, 1, PAGE(2), 0), PRV_U);
  CHECK_EQ(last_trap.a0, DATA + 2);
}

// A key on a pointer PTE, at either level, is a page fault of the access
// type, even with every right granted
static void check_nonleaf(void)
{
  setup();
  for (int k = 0; k < NKEYS; k++)
    rights_set(1, k, DR_R | DR_W | DR_X);
  rights_apply();
  place_stub(PAGE(5) + STUB_OFF, stub_ecall, stub_ecall_end);

  uint64_t *ptrs[2] = { &sv39_root(0)[(ALIAS >> 30) & 511], &l1[0] };
  for (int i = 0; i < 2; i++) {
    for (int key = 1; key < NKEYS; key++) {
      uint64_t saved = *ptrs[i];
      *ptrs[i] = pte_key(saved, key);
      CHECK_EQ(run_u(stub_ld, 1, APAGE(2), 0), CAUSE_LOAD_PAGE_FAULT);
      CHECK_EQ(last_trap.tval, APAGE(2));
      CHECK_EQ(run_u(stub_sd, 1, APAGE(2), 0), CAUSE_STORE_PAGE_FAULT);
      CHECK_EQ(run_u(stub_amoadd_d, 1, APAGE(2), 0), CAUSE_STORE_PAGE_FAULT);
      CHECK_EQ(run_u((void *)(APAGE(5) + STUB_OFF), 1, 0, 0), CAUSE_FETCH_PAGE_FAULT);
      CHECK_EQ(last_trap.tval, APAGE(5) + STUB_OFF);
      *ptrs[i] = saved;
    }
  }
  CHECK_OK(run_u(stub_ld, 1, APAGE(2), 0), PRV_U);
  CHECK_OK(run_u((void *)(APAGE(5) + STUB_OFF), 1, 0, 0), PRV_U);
}

// With dstatus.EN = 0 the key bits are reserved again: any key, on a leaf or
// a pointer, is a page fault
static void check_disabled(void)
{
  setup();
  place_stub(PAGE(5) + STUB_OFF, stub_ecall, stub_ecall_end);
  dom_disable();  // the identity map goes back to key 0; the alias keeps its keys

  CHECK_EQ(run_u(stub_ld, 1, APAGE(3), 0), CAUSE_LOAD_PAGE_FAULT);
  CHECK_EQ(last_trap.tval, APAGE(3));
  CHECK_EQ(run_u(stub_sd, 1, APAGE(3), 0), CAUSE_STORE_PAGE_FAULT);
  CHECK_EQ(run_u((void *)(APAGE(5) + STUB_OFF), 1, 0, 0), CAUSE_FETCH_PAGE_FAULT);
  CHECK_OK(run_u(stub_ld, 1, APAGE(0), 0), PRV_U);

  l0[3] = pte_key(l0[3], 0);
  CHECK_OK(run_u(stub_ld, 1, APAGE(3), 0), PRV_U);
  CHECK_EQ(last_trap.a0, DATA + 3);

  l1[0] = pte_key(l1[0], 1);
  CHECK_EQ(run_u(stub_ld, 1, APAGE(3), 0), CAUSE_LOAD_PAGE_FAULT);
}

enum { ACC_LOAD, ACC_STORE, ACC_AMO, ACC_FETCH };

// The base PTE checks come first: a PTE denial is a page fault whether DOM1
// has no rights on the key or all of them, so the rights never widen the PTE
// permissions
static void check_order(void)
{
  static const struct {
    uint64_t flags;
    int acc;
    uint64_t none, all;   // cause with no rights, with RWX on (1, 3)
  } cases[] = {
    { PTE_V | PTE_X | PTE_U | PTE_A | PTE_D,         ACC_LOAD,  CAUSE_LOAD_PAGE_FAULT,  CAUSE_LOAD_PAGE_FAULT },
    { PTE_V | PTE_R | PTE_U | PTE_A | PTE_D,         ACC_STORE, CAUSE_STORE_PAGE_FAULT, CAUSE_STORE_PAGE_FAULT },
    { PTE_V | PTE_R | PTE_U | PTE_A | PTE_D,         ACC_AMO,   CAUSE_STORE_PAGE_FAULT, CAUSE_STORE_PAGE_FAULT },
    { PTE_V | PTE_R | PTE_W | PTE_U | PTE_A | PTE_D, ACC_FETCH, CAUSE_FETCH_PAGE_FAULT, CAUSE_FETCH_PAGE_FAULT },
    { LEAF & ~PTE_A,                                 ACC_LOAD,  CAUSE_LOAD_PAGE_FAULT,  CAUSE_LOAD_PAGE_FAULT },
    { LEAF & ~PTE_D,                                 ACC_STORE, CAUSE_STORE_PAGE_FAULT, CAUSE_STORE_PAGE_FAULT },
    { LEAF & ~PTE_D,                                 ACC_LOAD,  CAUSE_DOMAIN_ACCESS,    CAUSE_ECALL_U },
    { LEAF & ~PTE_U,                                 ACC_LOAD,  CAUSE_LOAD_PAGE_FAULT,  CAUSE_LOAD_PAGE_FAULT },
    { LEAF,                                          ACC_LOAD,  CAUSE_DOMAIN_ACCESS,    CAUSE_ECALL_U },
  };

  setup();
  place_stub(PAGE(3) + STUB_OFF, stub_ecall, stub_ecall_end);
  for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    l0[3] = pte_key(pte(PAGE(3), cases[i].flags), 3);
    for (int all = 0; all < 2; all++) {
      rights_set(1, 3, all ? DR_R | DR_W | DR_X : 0);
      rights_apply();
      uintptr_t addr = APAGE(3);
      uint64_t cause;
      switch (cases[i].acc) {
      case ACC_LOAD:  cause = run_u(stub_ld, 1, addr, 0); break;
      case ACC_STORE: cause = run_u(stub_sd, 1, addr, 0); break;
      case ACC_AMO:   cause = run_u(stub_amoadd_d, 1, addr, 0); break;
      default:
        addr += STUB_OFF;
        cause = run_u((void *)addr, 1, 0, 0);
        break;
      }
      int before = failures;
      CHECK_EQ(cause, all ? cases[i].all : cases[i].none);
      if (cause != CAUSE_ECALL_U)
        CHECK_EQ(last_trap.tval, addr);
      if (failures != before)
        printf("    (case %d, %s rights)\n", (int)i, all ? "all" : "no");
    }
  }
}

// Under translation the key of the matching PMP entry is ignored
static void check_dmpcfg_ignored(void)
{
  setup();
  csr_write(CSR_DMPCFG0, 5);  // entry 0 covers all the memory
  rights_set(1, 2, DR_R);
  rights_apply();
  CHECK_OK(run_u(stub_ld, 1, PAGE(2), 0), PRV_U);
  CHECK_EQ(last_trap.a0, DATA + 2);

  rights_set(1, 2, 0);
  rights_set(1, 5, DR_R);
  rights_apply();
  CHECK_DOM_FAULT(run_u(stub_ld, 1, PAGE(2), 0), PAGE(2));
}

// A 2 MiB leaf gives its whole range its own key, whatever the 4 KiB leaves of
// the identity map say about the same physical pages
static void check_superpage(void)
{
  setup();
  l1[1] = pte_key(l1[1], 6);
  rights_set(1, 6, DR_R);
  rights_apply();
  CHECK_OK(run_u(stub_ld, 1, mega(PAGE(2)), 0), PRV_U);
  CHECK_EQ(last_trap.a0, DATA + 2);
  CHECK_OK(run_u(stub_ld, 1, mega(PAGE(5)), 0), PRV_U);
  CHECK_EQ(last_trap.a0, DATA + 5);

  rights_set(1, 6, 0);
  rights_set(1, 2, DR_R);
  rights_apply();
  CHECK_DOM_FAULT(run_u(stub_ld, 1, mega(PAGE(2)), 0), mega(PAGE(2)));
}

int main(void)
{
  check_alias();
  check_nonleaf();
  check_disabled();
  check_order();
  check_dmpcfg_ignored();
  check_superpage();
  return report("t-pte");
}
