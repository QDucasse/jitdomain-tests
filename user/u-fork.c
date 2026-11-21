// u-fork: fork copies the JITDomain state. The child has the process's EN,
// matrices and allocated keys, the pages they tag, and the forking thread's
// DRS, dsp and saved signal contexts: records pending at fork unwind in the
// child, and a handler that forks returns to the interrupted domain in both
// processes. The DRS of another thread stays mapped with the DRS key. After
// fork, a change of the matrices or the keys in one process does not reach
// the other.
//
// exec is not covered: under QEMU linux-user an exec'd program starts in a
// new QEMU, whose domain state is the reset state by construction.

#include <pthread.h>
#include <sys/wait.h>
#include "user.h"

#define GATE(from, to) (1ULL << (8 * (from) + (to)))

// in_dom1(): domcall into DOM1, which makes a syscall (refused with SIGSYS),
// then returns its curdom from after the ecall
__asm__(".text\n.balign 4\n.globl in_dom1\n"
        "in_dom1:\n"
        "  mv t1, ra\n"
        "  lla t0, 1f\n"
        "  .insn i 0x5b, 0, ra, t0, 512\n"
        "  mv ra, t1\n"
        "  ret\n"
        "  .balign 4\n"
        "1: .insn i 0x5b, 2, x0, x0, 512\n"
        "  li a7, 172\n"
        "  ecall\n"
        "  csrr a0, " CSR_STR(CSR_CURDOM) "\n"
        "  .insn i 0x5b, 1, x0, x0, 0\n");

// load_in_dom1(p): *p, loaded in DOM1
__asm__(".text\n.balign 4\n.globl load_in_dom1\n"
        "load_in_dom1:\n"
        "  mv t1, ra\n"
        "  lla t0, 1f\n"
        "  .insn i 0x5b, 0, ra, t0, 512\n"
        "  mv ra, t1\n"
        "  ret\n"
        "  .balign 4\n"
        "1: .insn i 0x5b, 2, x0, x0, 512\n"
        "  ld a0, 0(a0)\n"
        "  .insn i 0x5b, 1, x0, x0, 0\n");

// nested_fork(): DOM0 enters DOM1, which enters DOM0 again at label 1;
// there DOM0 forks, with two records on the DRS. Both processes unwind:
// .pid is fork's result, .dom1 DOM1's curdom after the inner domret.
struct nested { long pid; uint64_t dom1; };
__asm__(".text\n.balign 4\n.globl nested_fork\n"
        "nested_fork:\n"
        "  addi sp, sp, -16\n"
        "  sd ra, 8(sp)\n"
        "  lla t0, 1f\n"
        "  .insn i 0x5b, 0, ra, t0, 512\n"
        "  ld ra, 8(sp)\n"
        "  addi sp, sp, 16\n"
        "  ret\n"
        "  .balign 4\n"
        "1: .insn i 0x5b, 2, x0, x0, 512\n"
        "  lla t0, 2f\n"
        "  .insn i 0x5b, 0, ra, t0, 1\n"
        "  csrr a1, " CSR_STR(CSR_CURDOM) "\n"
        "  .insn i 0x5b, 1, x0, x0, 0\n"
        "  .balign 4\n"
        "2: .insn i 0x5b, 2, x0, x0, 1\n"
        "  call fork\n"
        "  .insn i 0x5b, 1, x0, x0, 0\n");
uint64_t in_dom1(void), load_in_dom1(volatile uint64_t *p);
struct nested nested_fork(void);

static uintptr_t top;
static long page;
static volatile uint64_t *p, *q;
static struct jitdomain_matrices m;
static volatile pid_t handler_pid = -1;

// The child's counts, for the parent to add to its own
static volatile struct { int checks, failures; } *shared;

static void child_start(void)
{
  checks = failures = 0;
}

static __attribute__((noreturn)) void child_done(void)
{
  shared->checks = checks;
  shared->failures = failures;
  _exit(0);
}

static void reap(pid_t pid)
{
  int st = 0;

  shared->checks = shared->failures = 0;
  CHECK_EQ(waitpid(pid, &st, 0), pid);
  CHECK_EQ(WIFEXITED(st) && WEXITSTATUS(st) == 0, 1);
  __atomic_fetch_add(&checks, shared->checks, __ATOMIC_RELAXED);
  __atomic_fetch_add(&failures, shared->failures, __ATOMIC_RELAXED);
}

// What both processes must still see after fork
static void check_inherited(void)
{
  struct jitdomain_matrices got;

  CHECK_EQ(csr_read(CSR_CURDOM), 0);
  CHECK_EQ(csr_read(CSR_UDSP), top);
  CHECK_EQ(jd_get(&got), 0);
  CHECK_MATRICES(got, m);
  // The tags: DOM1 reads key 1, DOM0 has nothing on key 2
  CHECK_EQ(load_in_dom1(p), 0x1234);
  CHECK_EQ(csr_read(CSR_UDSP), top);
  CHECK_EQ(TRY((void)*q), SIGSEGV);
  CHECK_EQ(last_fault.code, SEGV_PKUERR);
}

static void sigsys_handler(int sig, siginfo_t *si, void *uc)
{
  (void)sig, (void)si, (void)uc;
  handler_pid = fork();
}

// A second thread with its own DRS, waiting until the main thread is done
static int ready[2], release[2];
static volatile uintptr_t thread_top;

static void *thread_fn(void *arg)
{
  char c = 0;

  (void)arg;
  if (jd_prctl(PR_JITDOMAIN_DRS_ALLOC, 0, 0, 0) == 0)
    thread_top = csr_read(CSR_UDSP);
  (void)!write(ready[1], &c, 1);
  (void)!read(release[0], &c, 1);
  return NULL;
}

int main(void)
{
  struct jitdomain_matrices other, got;
  struct sigaction sa;
  struct nested n;
  pthread_t th;
  pid_t pid;
  uint64_t r;
  char c = 0;

  user_init();
  page = sysconf(_SC_PAGESIZE);
  shared = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  CHECK_EQ(shared == MAP_FAILED, 0);
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = sigsys_handler;
  sa.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigaction(SIGSYS, &sa, NULL);

  CHECK_EQ(jd_enable(), 0);
  CHECK_EQ(jd_prctl(PR_JITDOMAIN_DRS_ALLOC, 0, 0, 0), 0);
  top = csr_read(CSR_UDSP);
  CHECK_EQ(jd_pkey_alloc(0, 0), 1);
  CHECK_EQ(jd_pkey_alloc(0, 0), 2);
  p = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  q = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK_EQ(p == MAP_FAILED || q == MAP_FAILED, 0);
  *p = 0x1234;
  CHECK_EQ(jd_pkey_mprotect(p, page, PROT_READ | PROT_WRITE, 1), 0);
  CHECK_EQ(jd_pkey_mprotect(q, page, PROT_READ | PROT_WRITE, 2), 0);

  // DOM0: nothing on key 2. DOM1: X on key 0, R on key 1. DOM0 and DOM1
  // may enter each other
  m = permissive();
  set_rights(&m, 0, 2, 0);
  set_rights(&m, 1, 0, DR_X);
  set_rights(&m, 1, 1, DR_R);
  m.dgate = GATE(0, 1) | GATE(1, 0);
  CHECK_EQ(jd_set(&m), 0);

  // The process state and the DRS
  pid = fork();
  if (pid == 0) {
    child_start();
    check_inherited();
    child_done();
  }
  CHECK_EQ(pid > 0, 1);
  reap(pid);
  check_inherited();

  // Records pending at fork unwind in both processes
  n = nested_fork();
  if (n.pid == 0) {
    child_start();
    CHECK_EQ(n.dom1, 1);
    CHECK_EQ(csr_read(CSR_CURDOM), 0);
    CHECK_EQ(csr_read(CSR_UDSP), top);
    child_done();
  }
  CHECK_EQ(n.pid > 0, 1);
  reap(n.pid);
  CHECK_EQ(n.dom1, 1);
  CHECK_EQ(csr_read(CSR_UDSP), top);

  // A handler that forks returns to DOM1 in both processes
  r = in_dom1();
  if (handler_pid == 0) {
    child_start();
    CHECK_EQ(r, 1);
    CHECK_EQ(csr_read(CSR_CURDOM), 0);
    CHECK_EQ(csr_read(CSR_UDSP), top);
    child_done();
  }
  CHECK_EQ(handler_pid > 0, 1);
  reap(handler_pid);
  CHECK_EQ(r, 1);
  CHECK_EQ(csr_read(CSR_UDSP), top);

  // Another thread's DRS stays mapped, with the DRS key
  CHECK_EQ(pipe(ready), 0);
  CHECK_EQ(pipe(release), 0);
  CHECK_EQ(pthread_create(&th, NULL, thread_fn, NULL), 0);
  CHECK_EQ(read(ready[0], &c, 1), 1);
  CHECK_EQ(thread_top != 0, 1);
  pid = fork();
  if (pid == 0) {
    child_start();
    CHECK_EQ(TRY((void)*(volatile uint64_t *)(thread_top - 8)), SIGSEGV);
    CHECK_EQ(last_fault.code, SEGV_PKUERR);
    CHECK_EQ(last_fault.addr, thread_top - 8);
    child_done();
  }
  CHECK_EQ(pid > 0, 1);
  reap(pid);
  CHECK_EQ(write(release[1], &c, 1), 1);
  CHECK_EQ(pthread_join(th, NULL), 0);

  // A change in the child stays there
  pid = fork();
  if (pid == 0) {
    child_start();
    other = m;
    set_rights(&other, 1, 1, 0);
    CHECK_EQ(jd_set(&other), 0);
    CHECK_EQ(jd_pkey_alloc(0, 0), 3);
    child_done();
  }
  CHECK_EQ(pid > 0, 1);
  reap(pid);
  CHECK_EQ(jd_get(&got), 0);
  CHECK_MATRICES(got, m);
  CHECK_EQ(load_in_dom1(p), 0x1234);
  CHECK_EQ(jd_pkey_alloc(0, 0), 3);

  return user_done("u-fork");
}
