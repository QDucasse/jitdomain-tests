// KEYSRC=sv39: keys come from the leaf PTEs, bits [56:54].
//
// An identity map of [LOAD_ADDR, drs_top) with 4 KiB pages: the image with
// key 0, page k with key k, and the two DRS pages with key 7. The stacks above
// the DRS are not mapped, so S and U mode cannot reach them.
//
// S mode cannot fetch from U pages, so there are two copies of the tables, one
// with U pages and one with S pages, and run_lower() picks the one for the
// stub's privilege. One PMP entry covers [0, drs_top) with RWX; its key is
// ignored under translation.

#include "env.h"
#include "sv39.h"

#define PMP_RWX_TOR 0x0f

extern char LOAD_ADDR[];

// [U, S][root, level 1, level 0]
static uint64_t tables[2][3][512] __attribute__((aligned(PAGE_SIZE)));
uint64_t keysrc_satp[2];

static uint64_t *leaf(int t, uintptr_t va)
{
  return &tables[t][2][(va >> 12) & 511];
}

static void leaf_key(uintptr_t va, int key)
{
  for (int t = 0; t < 2; t++) {
    uint64_t *p = leaf(t, va);
    *p = pte_key(*p, key);
  }
}

uint64_t *sv39_root(int t)
{
  return tables[t][0];
}

void keysrc_reset(void)
{
  uintptr_t base = (uintptr_t)LOAD_ADDR, top = (uintptr_t)drs_top;

  // One level-0 table maps 2 MiB
  if (base >> 21 != (top - 1) >> 21) {
    printf("keysrc-sv39: the image does not fit in one 2 MiB region\n");
    exit(1);
  }

  for (int t = 0; t < 2; t++) {
    memset(tables[t], 0, sizeof(tables[t]));
    tables[t][0][(base >> 30) & 511] = pte((uintptr_t)tables[t][1], PTE_V);
    tables[t][1][(base >> 21) & 511] = pte((uintptr_t)tables[t][2], PTE_V);
    for (uintptr_t va = base; va < top; va += PAGE_SIZE)
      *leaf(t, va) = pte(va, PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D | (t == 0 ? PTE_U : 0));
    keysrc_satp[t] = SATP_SV39 | (uintptr_t)tables[t][0] >> 12;
  }

  csr_write(pmpaddr0, top >> 2);
  csr_write(pmpcfg0, PMP_RWX_TOR);
  csr_write(CSR_DMPCFG0, 0);

  for (uintptr_t va = (uintptr_t)drs_base; va < top; va += PAGE_SIZE)
    leaf_key(va, 7);
  for (int k = 0; k < NKEYS; k++)
    key_set(k, k);
}

// With EN = 0 the PTE key bits are reserved again, so every page goes back to
// key 0, the DRS included; dom_reset() restores the keys
void keysrc_disable(void)
{
  for (uintptr_t va = (uintptr_t)LOAD_ADDR; va < (uintptr_t)drs_top; va += PAGE_SIZE)
    leaf_key(va, 0);
  __asm__ volatile("sfence.vma" ::: "memory");
}

// Only page p: unlike KEYSRC=pmp, the image and the DRS keep their keys
void key_set(int page, int key)
{
  leaf_key(PAGE(page), key);
  __asm__ volatile("sfence.vma" ::: "memory");
}
