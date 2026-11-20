// u-sigsys: a syscall made outside DOM0 is not executed. The thread gets
// SIGSYS as for a seccomp SECCOMP_RET_TRAP: si_code SYS_SECCOMP, the
// syscall number, si_call_addr past the ecall, si_arch RISC-V 64, with pc
// past the ecall and a0 still the first argument. The handler runs in DOM0.
// Checked with the syscalls a
// domain would use to leave without a gate: changing the matrices,
// retagging a page, writing through the kernel; and with riscv_flush_icache,
// which QEMU handles apart.

#define _GNU_SOURCE
#include <fcntl.h>
#include <ucontext.h>
#include "user.h"

#define GATE(from, to) (1ULL << (8 * (from) + (to)))
#define AUDIT_ARCH_RISCV64 0xC00000F3u
#ifndef SYS_SECCOMP
#define SYS_SECCOMP 1
#endif
#ifndef SYS_riscv_flush_icache
#define SYS_riscv_flush_icache 259
#endif

static uintptr_t top;
static struct jitdomain_matrices m;

static volatile struct {
  int code, err, nr;
  unsigned arch;
  uintptr_t call_addr, pc, a0, dom;
} sys_info;

static void sigsys_handler(int sig, siginfo_t *si, void *ucv)
{
  ucontext_t *uc = ucv;

  sys_info.code = si->si_code;
  sys_info.err = si->si_errno;
  sys_info.nr = si->si_syscall;
  sys_info.arch = si->si_arch;
  sys_info.call_addr = (uintptr_t)si->si_call_addr;
  sys_info.pc = uc->uc_mcontext.__gregs[0];
  sys_info.a0 = uc->uc_mcontext.__gregs[10];
  sys_info.dom = csr_read(CSR_CURDOM) & CURDOM_DOM;
  last_fault.sig = sig;
  siglongjmp(fault_jb, 1);
}

// In DOMd: ecall with a7 = nr, a0-a2 = args, a3 = a4 = 0, then domret. at gets the
// address of the ecall.
#define IN_DOM_ECALL(d) \
  __asm__ volatile( \
      "lla %[at], 3f\n" \
      "lla t0, 1f\n" \
      ".insn i 0x5b, 0, ra, t0, %[imm]\n" \
      "j 2f\n" \
      ".balign 4\n" \
      "1: .insn i 0x5b, 2, x0, x0, %[imm]\n" \
      "3: ecall\n" \
      ".insn i 0x5b, 1, x0, x0, 0\n" \
      "2:\n" \
      : [at] "=&r"(at), "+r"(a0) \
      : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a7), [imm] "i"(DOM_IMM(d, 0)) \
      : "t0", "ra", "memory")

static __attribute__((noinline)) uintptr_t ecall_in(unsigned d, long nr, long x, long y, long z)
{
  register long a0 __asm__("a0") = x;
  register long a1 __asm__("a1") = y;
  register long a2 __asm__("a2") = z;
  register long a3 __asm__("a3") = 0;
  register long a4 __asm__("a4") = 0;
  register long a7 __asm__("a7") = nr;
  uintptr_t at = 0;

  if (d == 1)
    IN_DOM_ECALL(1);
  else
    IN_DOM_ECALL(7);
  return at;
}

static void set_udsp(uint64_t v)
{
  __asm__ volatile("csrw " CSR_STR(CSR_UDSP) ", %0" :: "r"(v) : "memory");
}

// From the matrices m, the syscall in DOMd must raise SIGSYS with these
// fields, in DOM0
static void check_sigsys(unsigned d, long nr, long x, long y, long z)
{
  static volatile uintptr_t at;
  int sig;

  CHECK_EQ(jd_set(&m), 0);

  memset((void *)&sys_info, 0, sizeof(sys_info));
  sys_info.dom = ~0UL;
  at = 0;
  sig = TRY(at = ecall_in(d, nr, x, y, z));
  CHECK_EQ(sig, SIGSYS);
  if (sig != SIGSYS) {
    // The syscall ran, or something else faulted: the frame is not ours
    if (csr_read(CSR_UDSP) != top)
      set_udsp(top);
    return;
  }
  at = sys_info.pc - 4;
  CHECK_EQ(sys_info.code, SYS_SECCOMP);
  CHECK_EQ(sys_info.err, 0);
  CHECK_EQ(sys_info.nr, nr);
  CHECK_EQ(sys_info.arch, AUDIT_ARCH_RISCV64);
  CHECK_EQ(sys_info.call_addr, at + 4);
  CHECK_EQ(sys_info.a0, x);
  CHECK_EQ(sys_info.dom, 0);
  CHECK_EQ(*(volatile uint32_t *)at, 0x00000073);   // the ecall itself
  CHECK_EQ(csr_read(CSR_CURDOM) & CURDOM_DOM, 0);
  CHECK_EQ(csr_read(CSR_UDSP), top - 8);
  set_udsp(top);
}

int main(void)
{
  struct jitdomain_matrices got, wide;
  struct sigaction sa;
  uint64_t *p;
  int pipefd[2];
  char c;
  long page;

  user_init();
  page = sysconf(_SC_PAGESIZE);
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = sigsys_handler;
  sa.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigaction(SIGSYS, &sa, NULL);

  CHECK_EQ(jd_enable(), 0);
  CHECK_EQ(jd_prctl(PR_JITDOMAIN_DRS_ALLOC, 0, 0, 0), 0);
  top = csr_read(CSR_UDSP);
  CHECK_EQ(jd_pkey_alloc(0, 0), 1);
  p = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK_EQ(p == MAP_FAILED, 0);
  CHECK_EQ(jd_pkey_mprotect(p, page, PROT_READ | PROT_WRITE, 1), 0);

  // DOM1 and DOM7 may run their code on key 0; DOM0 has nothing on key 1
  m = permissive();
  set_rights(&m, 0, 1, 0);
  set_rights(&m, 1, 0, DR_X);
  set_rights(&m, 7, 0, DR_X);
  m.dgate = GATE(0, 1) | GATE(0, 7);
  CHECK_EQ(jd_set(&m), 0);

  for (unsigned d = 1; d < NDOMS; d += 6) {
    int before = failures;

    // Widening the matrices
    wide = permissive();
    check_sigsys(d, SYS_prctl, PR_RISCV_JITDOMAIN, PR_JITDOMAIN_SET_MATRICES, (long)&wide);
    CHECK_EQ(jd_get(&got), 0);
    CHECK_MATRICES(got, m);

    // Retagging a page to key 0 (a3)
    check_sigsys(d, SYS_pkey_mprotect, (long)p, page, PROT_READ | PROT_WRITE);
    CHECK_EQ(TRY(*(volatile uint64_t *)p), SIGSEGV);
    CHECK_EQ(last_fault.code, SEGV_PKUERR);

    // Writing through the kernel
    CHECK_EQ(pipe2(pipefd, O_NONBLOCK), 0);
    check_sigsys(d, SYS_write, pipefd[1], (long)"x", 1);
    CHECK_EQ(read(pipefd[0], &c, 1), -1);
    CHECK_EQ(errno, EAGAIN);
    close(pipefd[0]);
    close(pipefd[1]);

    // The syscall QEMU handles before the others
    check_sigsys(d, SYS_riscv_flush_icache, 0, 0, 0);

    if (failures != before)
      printf("    (from DOM%u)\n", d);
  }

  // DOM0's syscalls still go through
  CHECK_EQ(getpid() > 0, 1);
  CHECK_EQ(jd_get(&got), 0);
  CHECK_MATRICES(got, m);

  return user_done("u-sigsys");
}
