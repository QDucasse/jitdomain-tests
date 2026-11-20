// u-matrix: PR_JITDOMAIN_SET_MATRICES and GET_MATRICES. Values read back
// as written; a right on the DRS key, dtarget bits above 23, non-zero
// unused arguments and bad pointers are rejected and change nothing. A new
// dgate reaches the other threads: a domcall first fails its gate check
// (domain CFI fault), then passes it and faults on the DRS push, since no
// thread has a DRS yet (dsp = 0).

#include <pthread.h>
#include "user.h"

static pthread_barrier_t barrier;

// domcall into DOM1: the signal and its si_code
static int domcall1(int *code)
{
  int sig = TRY(__asm__ volatile(
      "lla t0, 1f\n"
      ".insn i 0x5b, 0, ra, t0, %0\n"
      ".balign 4\n"
      "1: .insn i 0x5b, 2, x0, x0, %0\n"
      :: "i"(DOM_IMM(1, 0)) : "t0", "ra", "memory"));
  *code = last_fault.code;
  return sig;
}

static void check_gate(int open)
{
  int code;
  CHECK_EQ(domcall1(&code), SIGSEGV);
  CHECK_EQ(code, open ? SEGV_MAPERR : SEGV_CPERR);
}

static void *other_thread(void *arg)
{
  (void)arg;
  check_gate(0);
  pthread_barrier_wait(&barrier);  // main opens the gate
  pthread_barrier_wait(&barrier);
  check_gate(1);
  pthread_barrier_wait(&barrier);  // main closes it
  pthread_barrier_wait(&barrier);
  check_gate(0);
  return NULL;
}

// SET fails with err and the matrices stay as they were
static void check_rejected(const struct jitdomain_matrices *bad, long err)
{
  struct jitdomain_matrices before, after;
  CHECK_EQ(jd_get(&before), 0);
  CHECK_EQ(jd_set(bad), err);
  CHECK_EQ(jd_get(&after), 0);
  CHECK_MATRICES(after, before);
}

int main(void)
{
  struct jitdomain_matrices m, got, perm;
  pthread_t other;

  user_init();
  CHECK_EQ(jd_enable(), 0);
  perm = permissive();

  // Read back as written, rights on keys nobody allocated included
  m = perm;
  set_rights(&m, 1, 1, DR_R | DR_W);
  set_rights(&m, 1, 3, DR_R | DR_X);
  set_rights(&m, 2, 2, DR_R | DR_W);
  set_rights(&m, 7, 6, DR_X);
  m.dtarget = (dtarget_identity() & ~(7ULL << 3)) | (2ULL << 3);
  m.dtarget |= 7ULL << 21;
  m.dgate = 0xffffffffffffffffULL;
  CHECK_EQ(jd_set(&m), 0);
  CHECK_EQ(jd_get(&got), 0);
  CHECK_MATRICES(got, m);

  // A right on the DRS key, for any domain and any right
  for (unsigned d = 0; d < NDOMS; d++)
    for (unsigned r = DR_R; r <= DR_X; r <<= 1) {
      struct jitdomain_matrices bad = perm;
      set_rights(&bad, d, DRSKEY, r);
      check_rejected(&bad, -EINVAL);
    }

  // dtarget bits above the 8 3-bit fields
  for (unsigned b = 3 * NDOMS; b < 64; b += 13) {
    struct jitdomain_matrices bad = perm;
    bad.dtarget |= 1ULL << b;
    check_rejected(&bad, -EINVAL);
  }

  // Unused arguments and bad pointers
  CHECK_EQ(jd_prctl(PR_JITDOMAIN_SET_MATRICES, (long)&perm, 1, 0), -EINVAL);
  CHECK_EQ(jd_prctl(PR_JITDOMAIN_SET_MATRICES, (long)&perm, 0, 1), -EINVAL);
  CHECK_EQ(jd_prctl(PR_JITDOMAIN_GET_MATRICES, (long)&got, 1, 0), -EINVAL);
  CHECK_EQ(jd_prctl(PR_JITDOMAIN_GET_MATRICES, (long)&got, 0, 1), -EINVAL);
  check_rejected(NULL, -EFAULT);
  CHECK_EQ(jd_get(NULL), -EFAULT);

  // dgate as seen by domcall, in this thread and in another one
  CHECK_EQ(jd_set(&perm), 0);
  pthread_barrier_init(&barrier, NULL, 2);
  pthread_create(&other, NULL, other_thread, NULL);
  check_gate(0);

  m = perm;
  m.dgate = 1 << 1;   // DOM0 -> DOM1
  pthread_barrier_wait(&barrier);
  CHECK_EQ(jd_set(&m), 0);
  pthread_barrier_wait(&barrier);
  check_gate(1);

  pthread_barrier_wait(&barrier);
  CHECK_EQ(jd_set(&perm), 0);
  pthread_barrier_wait(&barrier);
  check_gate(0);
  pthread_join(other, NULL);

  return user_done("u-matrix");
}
