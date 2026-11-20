// Shared helpers for the Linux user-space tests: the JITDomain prctl and
// pkeys syscalls, checks, and catching the signals a faulting instruction
// raises.

#ifndef USER_H
#define USER_H

#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include "../env/jitdomain.h"

#define PR_RISCV_JITDOMAIN        0x4A444F4D
#define PR_JITDOMAIN_ENABLE       1
#define PR_JITDOMAIN_SET_MATRICES 2
#define PR_JITDOMAIN_GET_MATRICES 3
#define PR_JITDOMAIN_DRS_ALLOC    4

#define DRSKEY 7

#ifndef SEGV_PKUERR
#define SEGV_PKUERR 4
#endif
#ifndef SEGV_CPERR
#define SEGV_CPERR 10
#endif

struct jitdomain_matrices {
  uint64_t drights[3];
  uint64_t dtarget;
  uint64_t dgate;
};

// Raw syscalls: -errno on failure, as the kernel returns it
static inline long sys(long nr, long a0, long a1, long a2, long a3, long a4)
{
  long r = syscall(nr, a0, a1, a2, a3, a4);
  return r == -1 ? -errno : r;
}
#define jd_prctl(op, a3, a4, a5) sys(SYS_prctl, PR_RISCV_JITDOMAIN, op, a3, a4, a5)
#define jd_enable() jd_prctl(PR_JITDOMAIN_ENABLE, 0, 0, 0)
#define jd_set(m) jd_prctl(PR_JITDOMAIN_SET_MATRICES, (long)(m), 0, 0)
#define jd_get(m) jd_prctl(PR_JITDOMAIN_GET_MATRICES, (long)(m), 0, 0)
#define jd_pkey_alloc(f, r) sys(SYS_pkey_alloc, f, r, 0, 0, 0)
#define jd_pkey_free(k) sys(SYS_pkey_free, k, 0, 0, 0, 0)
#define jd_pkey_mprotect(a, l, p, k) sys(SYS_pkey_mprotect, (long)(a), l, p, k, 0)

#define CSR_STR_(x) #x
#define CSR_STR(x) CSR_STR_(x)
#define csr_read(csr) ({ uint64_t __v; __asm__ volatile("csrr %0, " CSR_STR(csr) : "=r"(__v) :: "memory"); __v; })

// .insn takes a signed 12-bit immediate
#define DOM_IMM(dom, label) (((((dom) << 9) | (label)) ^ 0x800) - 0x800)

static inline uint64_t dtarget_identity(void)
{
  uint64_t v = 0;
  for (uint64_t d = 0; d < NDOMS; d++)
    v |= d << (3 * d);
  return v;
}

// {X, W, R} of domain d on key k in a matrices struct
static inline void set_rights(struct jitdomain_matrices *m, unsigned d, unsigned k, unsigned r)
{
  for (unsigned b = 0; b < 3; b++) {
    unsigned bit = 3 * (NDOMS * d + k) + b;
    m->drights[bit / 64] &= ~(1ULL << (bit % 64));
    m->drights[bit / 64] |= (uint64_t)((r >> b) & 1) << (bit % 64);
  }
}

// What ENABLE installs: DOM0 RWX on keys 0-6, nothing else
static inline struct jitdomain_matrices permissive(void)
{
  struct jitdomain_matrices m = { .dtarget = dtarget_identity() };
  for (unsigned k = 0; k < DRSKEY; k++)
    set_rights(&m, 0, k, DR_R | DR_W | DR_X);
  return m;
}

extern int checks, failures;
extern __thread sigjmp_buf fault_jb;
extern __thread volatile struct fault { int sig, code; uintptr_t addr; } last_fault;

#define CHECK_EQ(got, want) do { \
    uint64_t __g = (uint64_t)(got), __w = (uint64_t)(want); \
    __atomic_fetch_add(&checks, 1, __ATOMIC_RELAXED); \
    if (__g != __w) { \
      __atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED); \
      printf("FAIL %s:%d: %s = 0x%lx, want 0x%lx\n", __FILE__, __LINE__, #got, \
             (unsigned long)__g, (unsigned long)__w); \
    } \
  } while (0)

#define CHECK_MATRICES(got, want) do { \
    for (int __i = 0; __i < 3; __i++) \
      CHECK_EQ((got).drights[__i], (want).drights[__i]); \
    CHECK_EQ((got).dtarget, (want).dtarget); \
    CHECK_EQ((got).dgate, (want).dgate); \
  } while (0)

// Run stmt, catching SIGSEGV and SIGILL; the signal number, or 0
#define TRY(stmt) ({ \
    last_fault.sig = 0; \
    if (!sigsetjmp(fault_jb, 1)) { stmt; } \
    last_fault.sig; \
  })

// Installs the handlers and exits if the hart lacks the extension; the
// test calls it first
void user_init(void);
// Prints the summary; the exit code of the test
int user_done(const char *name);

#endif
