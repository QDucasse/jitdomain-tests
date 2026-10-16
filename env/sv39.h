// Sv39 page-table entries, for keysrc-sv39.c and t-pte

#ifndef SV39_H
#define SV39_H

#include <stdint.h>

#define PTE_V 0x01
#define PTE_R 0x02
#define PTE_W 0x04
#define PTE_X 0x08
#define PTE_U 0x10
#define PTE_A 0x40
#define PTE_D 0x80
#define PTE_KEY_SHIFT 54
#define PTE_KEY (7ULL << PTE_KEY_SHIFT)
#define PTE_PPN_SHIFT 10

#define SATP_SV39 (8ULL << 60)

static inline uint64_t pte(uintptr_t pa, uint64_t flags)
{
  return (pa >> 12) << PTE_PPN_SHIFT | flags;
}

static inline uint64_t pte_key(uint64_t p, int key)
{
  return (p & ~PTE_KEY) | (uint64_t)key << PTE_KEY_SHIFT;
}

// Root table of the tree run_lower() uses for U stubs (t = 0) or S stubs (t = 1)
uint64_t *sv39_root(int t);

#endif
