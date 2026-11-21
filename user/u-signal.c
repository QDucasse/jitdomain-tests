// u-signal: what sigreturn restores. Delivery saves the interrupted curdom,
// pending entry and dsp where the thread cannot reach them, and runs the
// handler in DOM0. sigreturn restores them only when the frame is the one
// delivered and resumes the interrupted pc:
// - unchanged pc: back in the interrupted domain, pending entry included,
//   whatever the frame's JDSG record says;
// - modified pc, or a copy of the frame elsewhere: DOM0, with dsp as the
//   handler left it;
// - nested handlers each restore their own context, and the entries of
//   handlers left by siglongjmp are dropped; the handler may leave dsp below
//   its entry value, but a dsp above it is SIGSEGV/SEGV_CPERR with dcause
//   DSPRANGE;
// - the saved contexts hold 32 entries: under 32 newer ones, the oldest
//   resumes in DOM0.

#include <ucontext.h>
#include "user.h"

#define GATE(from, to) (1ULL << (8 * (from) + (to)))
#define JDSG_MAGIC 0x4A445347u
#define PEND_VALUE(dom, label) ((uint64_t)(0x1000 | ((label) << 3) | (dom)) << 3)
#define NSAVED 32

struct ctx_hdr { uint32_t magic, size; };
struct jd_state { uint64_t curdom, dsp, dcause; };

// in_domN(): domcall into DOMN, which makes a syscall (refused with SIGSYS),
// then returns its curdom from after the ecall. in_domN_ret is that
// instruction, in_domN_back the return site in DOM0.
#define STUB(n, imm) \
  __asm__(".text\n.balign 4\n.globl in_dom" #n "\n" \
          "in_dom" #n ":\n" \
          "  mv t1, ra\n" \
          "  lla t0, 1f\n" \
          "  .insn i 0x5b, 0, ra, t0, " #imm "\n" \
          "in_dom" #n "_back:\n" \
          "  mv ra, t1\n" \
          "  ret\n" \
          "  .balign 4\n" \
          "1: .insn i 0x5b, 2, x0, x0, " #imm "\n" \
          "  li a7, 172\n" \
          "  ecall\n" \
          "in_dom" #n "_ret:\n" \
          "  csrr a0, " CSR_STR(CSR_CURDOM) "\n" \
          "  .insn i 0x5b, 1, x0, x0, 0\n")
STUB(1, 512);
STUB(2, 1024);
uint64_t in_dom1(void), in_dom2(void);
extern char in_dom1_ret[], in_dom1_back[];

enum mode { M_RETURN, M_REDIRECT, M_RECORD, M_NEST, M_ORPHAN, M_SATURATE, M_FORGE, M_DSPUP, M_LAZY };

static volatile enum mode mode;
static volatile int depth, saturate_n;
static uintptr_t top;
static uint32_t *lazy;
static long page;
static __thread sigjmp_buf inner_jb;
static uint64_t forged[256] __attribute__((aligned(16)));

// What the handlers saw
static volatile struct {
  int sig, code;
  uintptr_t addr;
  int has_jd;
  struct jd_state st;           // the outermost handler's record
  uint64_t inner_dom, inner_ret, after_inner_dom, after_inner_dsp;
} h;

static struct jd_state *jd_record(ucontext_t *uc)
{
  uint8_t *fp = (uint8_t *)&uc->uc_mcontext.__fpregs;
  struct ctx_hdr *hd = (void *)(fp + 520);

  for (int i = 0; i < 8 && hd->magic; i++) {
    if (hd->magic == JDSG_MAGIC)
      return (void *)(hd + 1);
    hd = (void *)((uint8_t *)hd + hd->size);
  }
  return NULL;
}

static void set_udsp(uint64_t v)
{
  __asm__ volatile("csrw " CSR_STR(CSR_UDSP) ", %0" :: "r"(v) : "memory");
}

// sigreturn on a copy of the frame at sp; does not return
static __attribute__((noreturn)) void sigreturn_at(void *sp)
{
  __asm__ volatile("mv sp, %0\n li a7, 139\n ecall" :: "r"(sp) : "a7", "memory");
  __builtin_unreachable();
}

static void handler(int sig, siginfo_t *si, void *ucv)
{
  ucontext_t *uc = ucv;
  struct jd_state *st = jd_record(uc);
  int d = ++depth;

  if (d == 1) {
    h.sig = sig;
    h.code = si->si_code;
    h.addr = (uintptr_t)si->si_addr;
    h.has_jd = st != NULL;
    if (st)
      memcpy((void *)&h.st, st, sizeof(*st));
  }

  // Faults that are not part of a case end it, as in TRY
  if ((sig == SIGSEGV || sig == SIGILL) && mode != M_LAZY) {
    last_fault.sig = sig;
    depth = 0;
    siglongjmp(fault_jb, 1);
  }

  switch (mode) {
  case M_RETURN:
    break;
  case M_REDIRECT:
    uc->uc_mcontext.__gregs[0] = (uintptr_t)in_dom1_back;
    uc->uc_mcontext.__gregs[10] = 0x55;
    break;
  case M_RECORD:
    if (st) {
      st->curdom = 7;
      st->dsp = top;
    }
    break;
  case M_NEST:
    if (d == 1) {
      h.inner_ret = in_dom2();
      h.after_inner_dom = csr_read(CSR_CURDOM);
      h.after_inner_dsp = csr_read(CSR_UDSP);
    } else {
      h.inner_dom = st->curdom;
    }
    break;
  case M_ORPHAN:
    if (d == 1) {
      if (!sigsetjmp(inner_jb, 1))
        in_dom2();
      h.after_inner_dom = csr_read(CSR_CURDOM);
      h.after_inner_dsp = csr_read(CSR_UDSP);
    } else {
      h.inner_dom = st->curdom;
      depth--;
      siglongjmp(inner_jb, 1);
    }
    break;
  case M_SATURATE:
    if (d == 1) {
      for (volatile int i = 0; i < saturate_n; i++)
        if (!sigsetjmp(inner_jb, 1))
          raise(SIGUSR1);
    } else {
      depth--;
      siglongjmp(inner_jb, 1);
    }
    break;
  case M_FORGE:
    memcpy(forged, si, sizeof(forged));
    depth--;
    sigreturn_at(forged);
  case M_DSPUP:
    set_udsp(top);
    break;
  case M_LAZY:
    mprotect(lazy, page, PROT_READ | PROT_EXEC);
    break;
  }
  depth--;
}

// Runs fn in mode m; its result, ~0 if a fault ended the case. Leaves the
// DRS empty.
static uint64_t run(enum mode m, uint64_t (*fn)(void))
{
  static volatile uint64_t r;

  memset((void *)&h, 0, sizeof(h));
  mode = m;
  depth = 0;
  r = ~0ULL;
  TRY(r = fn());
  mode = M_RETURN;
  return r;
}

// domcall into DOM1 at the pad on the lazy page; its curdom
static uint64_t in_lazy(void)
{
  register uint64_t a0 __asm__("a0");
  __asm__ volatile(
      "mv t0, %1\n"
      ".insn i 0x5b, 0, ra, t0, 512\n"
      : "=r"(a0) : "r"(lazy) : "t0", "ra", "memory");
  return a0;
}

int main(void)
{
  struct jitdomain_matrices m;
  struct sigaction sa;
  uint64_t r;

  user_init();
  page = sysconf(_SC_PAGESIZE);
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = handler;
  sa.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigaction(SIGSYS, &sa, NULL);
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);
  sigaction(SIGUSR1, &sa, NULL);

  CHECK_EQ(jd_enable(), 0);
  CHECK_EQ(jd_prctl(PR_JITDOMAIN_DRS_ALLOC, 0, 0, 0), 0);
  top = csr_read(CSR_UDSP);

  // DOM1 and DOM2 run their code on key 0
  m = permissive();
  set_rights(&m, 1, 0, DR_X);
  set_rights(&m, 2, 0, DR_X);
  m.dgate = GATE(0, 1) | GATE(0, 2);
  CHECK_EQ(jd_set(&m), 0);

  // The handler returns: back in DOM1 after the ecall, which then returns
  r = run(M_RETURN, in_dom1);
  CHECK_EQ(h.sig, SIGSYS);
  CHECK_EQ(h.has_jd, 1);
  CHECK_EQ(h.st.curdom, 1);
  CHECK_EQ(h.st.dsp, top - 8);
  CHECK_EQ(r, 1);
  CHECK_EQ(csr_read(CSR_CURDOM), 0);
  CHECK_EQ(csr_read(CSR_UDSP), top);

  // Modified pc: DOM0, dsp as the handler left it
  r = run(M_REDIRECT, in_dom1);
  CHECK_EQ(h.sig, SIGSYS);
  CHECK_EQ(r, 0x55);
  CHECK_EQ(csr_read(CSR_CURDOM), 0);
  CHECK_EQ(csr_read(CSR_UDSP), top - 8);
  set_udsp(top);

  // The frame's record is not read back
  r = run(M_RECORD, in_dom1);
  CHECK_EQ(h.sig, SIGSYS);
  CHECK_EQ(r, 1);
  CHECK_EQ(csr_read(CSR_UDSP), top);

  // Nested: the handler enters DOM2, interrupted in turn; each sigreturn
  // restores its own context
  r = run(M_NEST, in_dom1);
  CHECK_EQ(h.inner_dom, 2);
  CHECK_EQ(h.inner_ret, 2);
  CHECK_EQ(h.after_inner_dom, 0);
  CHECK_EQ(h.after_inner_dsp, top - 8);
  CHECK_EQ(r, 1);
  CHECK_EQ(csr_read(CSR_UDSP), top);

  // The inner handler leaves by siglongjmp: its entry is dropped, and the
  // outer one returns with DOM2's record still on the DRS, below its entry
  // dsp
  r = run(M_ORPHAN, in_dom1);
  CHECK_EQ(h.inner_dom, 2);
  CHECK_EQ(h.after_inner_dom, 0);
  CHECK_EQ(h.after_inner_dsp, top - 16);
  CHECK_EQ(r, 1);
  CHECK_EQ(csr_read(CSR_UDSP), top);

  // 31 newer entries left by siglongjmp keep the outer one, 32 drop it
  saturate_n = NSAVED - 1;
  r = run(M_SATURATE, in_dom1);
  CHECK_EQ(r, 1);
  CHECK_EQ(csr_read(CSR_UDSP), top);
  saturate_n = NSAVED;
  r = run(M_SATURATE, in_dom1);
  CHECK_EQ(r, 0);
  CHECK_EQ(csr_read(CSR_UDSP), top);

  // sigreturn on a copy of the frame: DOM0
  r = run(M_FORGE, in_dom1);
  CHECK_EQ(h.sig, SIGSYS);
  CHECK_EQ(r, 0);
  CHECK_EQ(csr_read(CSR_UDSP), top);

  // A handler that moves dsp above its entry value: nothing restored,
  // SIGSEGV/SEGV_CPERR at the frame's pc with dcause DSPRANGE
  r = run(M_DSPUP, in_dom1);
  CHECK_EQ(r, ~0ULL);
  CHECK_EQ(last_fault.sig, SIGSEGV);
  CHECK_EQ(h.sig, SIGSEGV);                   // the SIGSYS handler has returned
  CHECK_EQ(h.code, SEGV_CPERR);
  CHECK_EQ(h.addr, (uintptr_t)in_dom1_ret);
  CHECK_EQ(h.st.curdom, 0);
  CHECK_EQ(h.st.dsp, top);
  CHECK_EQ(h.st.dcause, DCAUSE_DSPRANGE);
  CHECK_EQ(csr_read(CSR_CURDOM), 0);
  CHECK_EQ(csr_read(CSR_UDSP), top);

  // A pending entry is restored: the pad's page faults when domcall lands
  // on it, the handler maps it, and the thread resumes at the pad with the
  // entry still pending
  lazy = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK_EQ(lazy == MAP_FAILED, 0);
  lazy[0] = 0x2000205b;                       // domlpad DOM1, label 0
  lazy[1] = 0xcc002573;                       // csrr a0, curdom
  lazy[2] = 0x0000105b;                       // domret
  __builtin___clear_cache((char *)lazy, (char *)(lazy + 3));
  CHECK_EQ(mprotect(lazy, page, PROT_NONE), 0);
  r = run(M_LAZY, in_lazy);
  CHECK_EQ(h.sig, SIGSEGV);
  CHECK_EQ(h.code, SEGV_ACCERR);
  CHECK_EQ(h.addr, lazy);
  CHECK_EQ(h.st.curdom, PEND_VALUE(1, 0));
  CHECK_EQ(r, 1);
  CHECK_EQ(csr_read(CSR_UDSP), top);

  return user_done("u-signal");
}
