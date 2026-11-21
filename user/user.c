#include "user.h"

int checks, failures;
__thread sigjmp_buf fault_jb;
__thread volatile int fault_armed;
__thread volatile struct fault last_fault;

void fault_return(int sig)
{
  if (!fault_armed) {
    printf("FAIL: signal %d outside TRY\n", sig);
    _exit(2);
  }
  fault_armed = 0;
  siglongjmp(fault_jb, 1);
}

static void handler(int sig, siginfo_t *si, void *uc)
{
  (void)uc;
  last_fault.sig = sig;
  last_fault.code = si->si_code;
  last_fault.addr = (uintptr_t)si->si_addr;
  fault_return(sig);
}

void user_init(void)
{
  struct sigaction sa;

  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = handler;
  sa.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);
  setvbuf(stdout, NULL, _IONBF, 0);

  // curdom exists in U mode whenever the hart has the extension
  if (TRY(csr_read(CSR_CURDOM))) {
    printf("no xjitdomain: curdom is an illegal instruction\n");
    exit(1);
  }
}

int user_done(const char *name)
{
  printf("%s: %d checks, %d failures\n", name, checks, failures);
  return failures != 0;
}
