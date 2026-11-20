// u-access: what the matrices and the page keys do to DOM0's accesses.
// Loads need R, stores W, LR and AMOs both, typed accesses are checked for
// dtarget[DOM0], fetches need X, and cbo.zero/cbo.clean need W. A denied
// access raises SIGSEGV/SEGV_PKUERR at the page it hits, unless the page
// protection refuses it first. mprotect keeps a page's key, and the kernel
// accessing tagged memory for a syscall is not checked.

#include <pthread.h>
#include "user.h"

static long page;
static struct jitdomain_matrices perm;
static pthread_barrier_t barrier;

#define CHECK_OK(stmt) CHECK_EQ(TRY(stmt), 0)
#define CHECK_SIG(stmt, si_code, at) do { \
    CHECK_EQ(TRY(stmt), SIGSEGV); \
    CHECK_EQ(last_fault.code, si_code); \
    CHECK_EQ(last_fault.addr, (uintptr_t)(at)); \
  } while (0)
#define CHECK_PKU(stmt, at) CHECK_SIG(stmt, SEGV_PKUERR, at)

static inline uint64_t ld(const void *p)
{
  uint64_t v;
  __asm__ volatile("ld %0, 0(%1)" : "=r"(v) : "r"(p) : "memory");
  return v;
}

static inline void sd(void *p, uint64_t v)
{
  __asm__ volatile("sd %0, 0(%1)" :: "r"(v), "r"(p) : "memory");
}

static inline uint64_t amoadd(void *p, uint64_t v)
{
  uint64_t old;
  __asm__ volatile("amoadd.d %0, %2, (%1)" : "=r"(old) : "r"(p), "r"(v) : "memory");
  return old;
}

static inline uint64_t lr(void *p)
{
  uint64_t v;
  __asm__ volatile("lr.d %0, (%1)" : "=r"(v) : "r"(p) : "memory");
  return v;
}

static inline uint64_t dld(const void *p)
{
  uint64_t v;
  __asm__ volatile(".insn i 0x0b, 3, %0, 0(%1)" : "=r"(v) : "r"(p) : "memory");
  return v;
}

static inline void dsd(void *p, uint64_t v)
{
  __asm__ volatile(".insn s 0x2b, 3, %0, 0(%1)" :: "r"(v), "r"(p) : "memory");
}

static inline double fld(const void *p)
{
  double v;
  __asm__ volatile("fld %0, 0(%1)" : "=f"(v) : "r"(p) : "memory");
  return v;
}

static inline void cbo_zero(void *p)
{
  __asm__ volatile(".insn i 0x0f, 2, x0, %0, 4" :: "r"(p) : "memory");
}

static inline void cbo_clean(void *p)
{
  __asm__ volatile(".insn i 0x0f, 2, x0, %0, 1" :: "r"(p) : "memory");
}

// DOM0's rights on key k, the permissive ones elsewhere
static void dom0_rights(unsigned k, unsigned r)
{
  struct jitdomain_matrices m = perm;
  set_rights(&m, 0, k, r);
  CHECK_EQ(jd_set(&m), 0);
}

static void *other_thread(void *arg)
{
  uint64_t *p = arg;
  CHECK_OK(ld(p));
  pthread_barrier_wait(&barrier);  // main takes R away
  pthread_barrier_wait(&barrier);
  CHECK_PKU(ld(p), p);
  return NULL;
}

int main(void)
{
  uint64_t *p, *q, v;
  char *two, *code;
  long (*fn)(void);
  int pipefd[2];
  pthread_t other;

  user_init();
  page = sysconf(_SC_PAGESIZE);
  perm = permissive();
  CHECK_EQ(jd_enable(), 0);
  CHECK_EQ(jd_pkey_alloc(0, 0), 1);
  CHECK_EQ(jd_pkey_alloc(0, 0), 2);

  p = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK_EQ(p == MAP_FAILED, 0);
  CHECK_EQ(jd_pkey_mprotect(p, page, PROT_READ | PROT_WRITE, 1), 0);
  q = p + 8;

  // RWX: everything goes through
  CHECK_OK(sd(p, 0x1234));
  CHECK_OK(v = ld(p));
  CHECK_EQ(v, 0x1234);
  CHECK_OK(amoadd(p, 1));
  CHECK_OK(lr(p));
  CHECK_EQ(ld(p), 0x1235);

  // R only: loads pass, stores and AMOs fault and change nothing
  dom0_rights(1, DR_R);
  CHECK_OK(v = ld(q));
  CHECK_OK(fld(q));
  CHECK_PKU(sd(q, 1), q);
  CHECK_PKU(amoadd(p, 1), p);
  CHECK_PKU(lr(p), p);
  dom0_rights(1, DR_R | DR_W);
  CHECK_EQ(ld(p), 0x1235);
  CHECK_EQ(ld(q), 0);

  // W only: stores pass, loads and AMOs fault
  dom0_rights(1, DR_W);
  CHECK_OK(sd(q, 7));
  CHECK_PKU(ld(q), q);
  CHECK_PKU(fld(q), q);
  CHECK_PKU(amoadd(p, 1), p);
  CHECK_PKU(lr(p), p);

  // Nothing; X alone is no data right
  dom0_rights(1, DR_X);
  CHECK_PKU(ld(p), p);
  CHECK_PKU(sd(p, 1), p);
  dom0_rights(1, DR_R | DR_W);
  CHECK_EQ(ld(p), 0x1235);
  CHECK_EQ(ld(q), 7);

  // Key 0 is unaffected, and the rights apply to keys, not pages
  dom0_rights(2, 0);
  CHECK_OK(ld(p));
  CHECK_OK(ld(&v));

  // cbo.zero and cbo.clean need W, whatever cbo.clean's probe type
  if (TRY(cbo_zero(p)) == SIGILL) {
    printf("skipping cbo: no Zicboz/Zicbom\n");
  } else {
    CHECK_EQ(ld(p), 0);
    sd(p, 0x55);
    dom0_rights(1, DR_R);
    CHECK_PKU(cbo_zero(p), p);
    CHECK_PKU(cbo_clean(p), p);
    dom0_rights(1, DR_W);
    CHECK_OK(cbo_clean(p));
    dom0_rights(1, DR_R | DR_W);
    CHECK_EQ(ld(p), 0x55);
  }

  // Typed accesses use dtarget[DOM0] = DOM1, which gets R on key 1 while
  // DOM0 gets nothing
  {
    struct jitdomain_matrices m = perm;
    set_rights(&m, 0, 1, 0);
    set_rights(&m, 1, 1, DR_R);
    m.dtarget = (dtarget_identity() & ~7ULL) | 1;
    CHECK_EQ(jd_set(&m), 0);
    CHECK_OK(v = dld(q));
    CHECK_EQ(v, 7);
    CHECK_PKU(ld(q), q);
    CHECK_PKU(dsd(q, 8), q);
    set_rights(&m, 1, 1, DR_R | DR_W);
    CHECK_EQ(jd_set(&m), 0);
    CHECK_OK(dsd(q, 8));
    CHECK_OK(v = dld(q));
    CHECK_EQ(v, 8);
    // Typed accesses to key 0 are checked for DOM1 too, which has nothing
    CHECK_PKU(dld(&perm), &perm);
    CHECK_EQ(jd_set(&perm), 0);
  }

  // The page protection is checked first: a store to a read-only page is
  // SEGV_ACCERR even without the right
  CHECK_EQ(mprotect(p, page, PROT_READ), 0);
  dom0_rights(1, 0);
  CHECK_SIG(sd(p, 1), SEGV_ACCERR, p);
  CHECK_PKU(ld(p), p);

  // mprotect keeps the key, pkey_mprotect to key 0 drops it
  CHECK_EQ(mprotect(p, page, PROT_READ | PROT_WRITE), 0);
  CHECK_PKU(ld(p), p);
  CHECK_EQ(jd_pkey_mprotect(p, page, PROT_READ | PROT_WRITE, 0), 0);
  CHECK_OK(ld(p));
  CHECK_EQ(jd_pkey_mprotect(p, page, PROT_READ | PROT_WRITE, 1), 0);
  CHECK_PKU(ld(p), p);

  // The kernel's own accesses for a syscall are not checked
  CHECK_EQ(pipe(pipefd), 0);
  CHECK_EQ(write(pipefd[1], p, 8), 8);
  CHECK_EQ(read(pipefd[0], q, 8), 8);
  close(pipefd[0]);
  close(pipefd[1]);
  dom0_rights(1, DR_R | DR_W);
  CHECK_EQ(ld(q), ld(p));

  // An access across two pages is checked on both, and faults at the
  // start of the page it is denied on
  two = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK_EQ(two == MAP_FAILED, 0);
  CHECK_EQ(jd_pkey_mprotect(two + page, page, PROT_READ | PROT_WRITE, 2), 0);
  CHECK_OK(ld(two + page - 4));
  dom0_rights(2, DR_W);
  CHECK_PKU(ld(two + page - 4), two + page);
  CHECK_OK(sd(two + page - 4, 1));
  CHECK_EQ(jd_set(&perm), 0);

  // Fetches need X, and X alone is enough
  code = mmap(NULL, page, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK_EQ(code == MAP_FAILED, 0);
  ((uint32_t *)code)[0] = 0x02a00513;   // li a0, 42
  ((uint32_t *)code)[1] = 0x00008067;   // ret
  __builtin___clear_cache(code, code + 8);
  CHECK_EQ(jd_pkey_mprotect(code, page, PROT_READ | PROT_WRITE | PROT_EXEC, 2), 0);
  fn = (long (*)(void))code;
  CHECK_OK(v = fn());
  CHECK_EQ(v, 42);
  dom0_rights(2, DR_R | DR_W);
  CHECK_PKU(fn(), code);
  dom0_rights(2, DR_X);
  CHECK_OK(v = fn());
  CHECK_EQ(v, 42);
  CHECK_EQ(jd_set(&perm), 0);

  // A change reaches a thread that is already running
  pthread_barrier_init(&barrier, NULL, 2);
  pthread_create(&other, NULL, other_thread, p);
  pthread_barrier_wait(&barrier);
  dom0_rights(1, DR_W);
  pthread_barrier_wait(&barrier);
  pthread_join(other, NULL);
  CHECK_PKU(ld(p), p);

  return user_done("u-access");
}
