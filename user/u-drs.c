// u-drs: PR_JITDOMAIN_DRS_ALLOC. It fails before ENABLE, with non-zero
// unused arguments, for a size that cannot be mapped, and a second time in
// the same thread. It maps the size rounded up to pages with the DRS key,
// between two PROT_NONE guard pages, which a later mmap does not take, and
// sets dsp to the top, which udsp reads. DOM0 cannot touch the DRS,
// pkey_mprotect refuses it, and a domcall/domret round trip goes through
// it. Overflow and underflow land in the guard pages (SEGV_ACCERR at the
// record address). New threads start without a DRS, their DRS and guard
// pages are unmapped when they exit, and a mapping over the DRS loses the
// DRS key: the next push is a DRS fault.

#include <pthread.h>
#include "user.h"

#define GATE(from, to) (1ULL << (8 * (from) + (to)))
#define DRS_DEFAULT (64 * 1024)

static long page;

#define CHECK_SIG(stmt, si_code, at) do { \
    CHECK_EQ(TRY(stmt), SIGSEGV); \
    CHECK_EQ(last_fault.code, si_code); \
    CHECK_EQ(last_fault.addr, (uintptr_t)(at)); \
  } while (0)

#define drs_alloc(size) jd_prctl(PR_JITDOMAIN_DRS_ALLOC, (long)(size), 0, 0)
#define udsp() csr_read(CSR_UDSP)

static inline uint64_t ld(uintptr_t p)
{
  uint64_t v;
  __asm__ volatile("ld %0, 0(%1)" : "=r"(v) : "r"(p) : "memory");
  return v;
}

// Whether a page is mapped: mprotect fails with ENOMEM on a hole. It
// leaves the page PROT_NONE, which a guard page already is.
static int mapped(uintptr_t p)
{
  return sys(SYS_mprotect, (long)p, page, PROT_NONE, 0, 0) != -ENOMEM;
}

// domcall into DOM1, whose pad reads curdom, then domret: curdom in DOM1
static uint64_t round_trip(void)
{
  uint64_t dom = ~0ULL;
  __asm__ volatile(
      "lla t0, 1f\n"
      ".insn i 0x5b, 0, ra, t0, %1\n"
      "j 2f\n"
      ".balign 4\n"
      "1: .insn i 0x5b, 2, x0, x0, %1\n"
      "csrr %0, " CSR_STR(CSR_CURDOM) "\n"
      ".insn i 0x5b, 1, x0, x0, 0\n"
      "2:\n"
      : "+r"(dom) : "i"(DOM_IMM(1, 0)) : "t0", "ra", "memory");
  return dom;
}

// domcalls from DOM0 into DOM0 until a push faults
static void push_forever(void)
{
  __asm__ volatile(
      "1: lla t0, 2f\n"
      ".insn i 0x5b, 0, ra, t0, %0\n"
      ".balign 4\n"
      "2: .insn i 0x5b, 2, x0, x0, %0\n"
      "j 1b\n"
      :: "i"(DOM_IMM(0, 0)) : "t0", "ra", "memory");
}

static void domret(void)
{
  __asm__ volatile(".insn i 0x5b, 1, x0, x0, 0" ::: "ra", "memory");
}

static void set_udsp(uint64_t v)
{
  __asm__ volatile("csrw " CSR_STR(CSR_UDSP) ", %0" :: "r"(v) : "memory");
}

// A new thread: no DRS, then a one-page DRS, filled to the lower guard
// page and emptied to the upper one
static void *small_drs(void *arg)
{
  uintptr_t top;

  (void)arg;
  CHECK_EQ(udsp(), 0);
  CHECK_EQ(drs_alloc(1), 0);
  top = udsp();
  CHECK_EQ(top % page, 0);

  // 512 records fit, the 513th push hits the guard page below
  CHECK_SIG(push_forever(), SEGV_ACCERR, top - page - 8);
  CHECK_EQ(udsp(), top - page);

  // Popping at the top reads the guard page above
  set_udsp(top);
  CHECK_SIG(domret(), SEGV_ACCERR, top);
  CHECK_EQ(udsp(), top);
  return (void *)top;
}

// A mapping over the DRS takes its pages: the push is a DRS fault, and the
// thread's exit leaves the new mapping alone
static void *replaced_drs(void *arg)
{
  uintptr_t top, base;
  void *p;

  (void)arg;
  CHECK_EQ(drs_alloc(page), 0);
  top = udsp();
  base = top - page;
  p = mmap((void *)base, page, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  CHECK_EQ(p, base);
  CHECK_EQ(jd_pkey_mprotect(base, page, PROT_READ | PROT_WRITE, 0), 0);
  CHECK_EQ(TRY(push_forever()), SIGSEGV);
  CHECK_EQ(last_fault.code, SEGV_CPERR);
  CHECK_EQ(udsp(), top);
  return (void *)base;
}

int main(void)
{
  struct jitdomain_matrices m;
  pthread_t t;
  uintptr_t top, other;
  void *ret, *hint;

  user_init();
  page = sysconf(_SC_PAGESIZE);

  // Before ENABLE
  CHECK_EQ(TRY((void)udsp()), SIGILL);   // udsp is illegal with EN = 0
  CHECK_EQ(drs_alloc(0), -EINVAL);

  CHECK_EQ(jd_enable(), 0);
  CHECK_EQ(jd_prctl(PR_JITDOMAIN_DRS_ALLOC, 0, 1, 0), -EINVAL);
  CHECK_EQ(jd_prctl(PR_JITDOMAIN_DRS_ALLOC, 0, 0, 1), -EINVAL);
  CHECK_EQ(drs_alloc(-1L), -ENOMEM);
  CHECK_EQ(udsp(), 0);

  // The default size, between two guard pages
  CHECK_EQ(drs_alloc(0), 0);
  top = udsp();
  CHECK_EQ(top % page, 0);
  CHECK_EQ(mapped(top - DRS_DEFAULT - page), 1);
  CHECK_EQ(mapped(top), 1);
  CHECK_SIG(ld(top), SEGV_ACCERR, top);
  CHECK_SIG(ld(top - DRS_DEFAULT - 8), SEGV_ACCERR, top - DRS_DEFAULT - 8);

  // A mapping asking for a guard page gets another address
  hint = mmap((void *)(top - DRS_DEFAULT - page), page, PROT_READ | PROT_WRITE,
              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK_EQ(hint != MAP_FAILED, 1);
  CHECK_EQ((uintptr_t)hint != top - DRS_DEFAULT - page, 1);
  munmap(hint, page);

  // DRS pages: no ordinary access, no retagging
  CHECK_SIG(ld(top - 8), SEGV_PKUERR, top - 8);
  CHECK_SIG(ld(top - DRS_DEFAULT), SEGV_PKUERR, top - DRS_DEFAULT);
  CHECK_EQ(jd_pkey_mprotect(top - page, page, PROT_READ | PROT_WRITE, 0), -EINVAL);

  // One per thread
  CHECK_EQ(drs_alloc(0), -EBUSY);
  CHECK_EQ(udsp(), top);

  // A round trip into DOM1
  m = permissive();
  set_rights(&m, 1, 0, DR_X);
  m.dgate = GATE(0, 0) | GATE(0, 1);
  CHECK_EQ(jd_set(&m), 0);
  CHECK_EQ(round_trip(), 1);
  CHECK_EQ(csr_read(CSR_CURDOM) & CURDOM_DOM, 0);
  CHECK_EQ(udsp(), top);

  // Threads: their own DRS, gone when they exit
  CHECK_EQ(pthread_create(&t, NULL, small_drs, NULL), 0);
  CHECK_EQ(pthread_join(t, &ret), 0);
  other = (uintptr_t)ret;
  CHECK_EQ(other != top, 1);
  CHECK_EQ(mapped(other - page), 0);
  CHECK_EQ(mapped(other - 2 * page), 0);   // the guard pages
  CHECK_EQ(mapped(other), 0);
  CHECK_EQ(udsp(), top);

  CHECK_EQ(pthread_create(&t, NULL, replaced_drs, NULL), 0);
  CHECK_EQ(pthread_join(t, &ret), 0);
  CHECK_EQ(mapped((uintptr_t)ret), 1);

  return user_done("u-drs");
}
