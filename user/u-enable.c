// u-enable: PR_JITDOMAIN_ENABLE. Before it, only GET_MATRICES works and
// udsp is an illegal instruction (u-pkey checks the pkeys syscalls). It enables the whole process, threads
// that already exist included, and installs the permissive matrices; a
// second call changes nothing.

#include <pthread.h>
#include "user.h"

static pthread_barrier_t barrier;

// udsp from this thread: the signal (0 if none) and the value read
static int read_udsp(uint64_t *v)
{
  uint64_t val = ~0ULL;
  int sig = TRY(val = csr_read(CSR_UDSP));
  *v = val;
  return sig;
}

static void *early_thread(void *arg)
{
  uint64_t v;
  (void)arg;
  CHECK_EQ(read_udsp(&v), SIGILL);
  pthread_barrier_wait(&barrier);  // main enables
  pthread_barrier_wait(&barrier);
  CHECK_EQ(read_udsp(&v), 0);
  CHECK_EQ(v, 0);
  return NULL;
}

static void *late_thread(void *arg)
{
  uint64_t v;
  (void)arg;
  CHECK_EQ(read_udsp(&v), 0);
  CHECK_EQ(v, 0);
  return NULL;
}

int main(void)
{
  struct jitdomain_matrices m, reset = { .dtarget = dtarget_identity() };
  struct jitdomain_matrices perm = permissive();
  pthread_t early, late;
  uint64_t v;

  user_init();
  pthread_barrier_init(&barrier, NULL, 2);
  pthread_create(&early, NULL, early_thread, NULL);

  // Before ENABLE: GET returns the reset values, everything else fails
  memset(&m, 0xa5, sizeof(m));
  CHECK_EQ(jd_get(&m), 0);
  CHECK_MATRICES(m, reset);
  CHECK_EQ(jd_set(&perm), -EINVAL);
  CHECK_EQ(read_udsp(&v), SIGILL);
  CHECK_EQ(csr_read(CSR_CURDOM), 0);

  // Unused arguments must be 0, unknown operations are rejected
  CHECK_EQ(jd_prctl(PR_JITDOMAIN_ENABLE, 1, 0, 0), -EINVAL);
  CHECK_EQ(jd_prctl(PR_JITDOMAIN_ENABLE, 0, 1, 0), -EINVAL);
  CHECK_EQ(jd_prctl(PR_JITDOMAIN_ENABLE, 0, 0, 1), -EINVAL);
  CHECK_EQ(jd_prctl(0, 0, 0, 0), -EINVAL);
  CHECK_EQ(jd_prctl(99, 0, 0, 0), -EINVAL);
  CHECK_EQ(read_udsp(&v), SIGILL);

  pthread_barrier_wait(&barrier);
  CHECK_EQ(jd_enable(), 0);
  pthread_barrier_wait(&barrier);

  // The permissive matrices; udsp reads dsp, 0 without a DRS
  CHECK_EQ(jd_get(&m), 0);
  CHECK_MATRICES(m, perm);
  CHECK_EQ(read_udsp(&v), 0);
  CHECK_EQ(v, 0);
  CHECK_EQ(csr_read(CSR_CURDOM), 0);
  pthread_join(early, NULL);

  pthread_create(&late, NULL, late_thread, NULL);
  pthread_join(late, NULL);

  // A second ENABLE succeeds and keeps the matrices
  struct jitdomain_matrices mine = perm;
  mine.dgate = 1 << 1;
  CHECK_EQ(jd_set(&mine), 0);
  CHECK_EQ(jd_enable(), 0);
  CHECK_EQ(jd_get(&m), 0);
  CHECK_MATRICES(m, mine);

  return user_done("u-enable");
}
