// u-jit: a small VM and its JIT code, following the runtime obligations.
// The VM (C, DOM0) generates methods into a code page with key 1 and runs
// them in DOM1, which may only execute that page and access the object heap
// (key 2); the VM's own code, data and stacks (key 0) are out of its reach.
// - The VM enters a method with domcall to its wildcard pad (DOM1, label 0).
// - Methods call primitives through a padded DOM0 trampoline with label 1,
//   and leave for the executive through one with label 2, a one-way domcall
//   followed by unimp. A domcall with the wrong label faults.
// - Primitives follow heap pointers with typed loads, and dtarget[0] = 1: a
//   pointer to VM data gets DOM1's rights, none.
// - Each entry into the VM's executive saves dsp as a base; leaving by an
//   exit to the executive resets udsp to the innermost base. A primitive
//   that calls back into JIT code is such an entry, and the outer method
//   resumes after its exit. Resetting past the innermost base is caught when
//   the inner level restores its value (DSPRANGE).
// - Only DOM0 patches the code. A signal handler on an alternate stack
//   interrupts a method and returns into it. A thread the VM did not create
//   allocates its DRS when udsp reads 0.

#include <pthread.h>
#include "user.h"

#define GATE(from, to) (1ULL << (8 * (from) + (to)))

// The three doors between the VM and generated code, on key 0. In assembly
// since C cannot emit the domain instructions; uncompressed to keep the
// pads 4-byte aligned.
__asm__(".text\n.option push\n.option norvc\n"
        // vm_enter(method, a0, a1): VM -> method, back with its result.
        // Method pads are wildcards (DOM1, label 0)
        ".balign 4\n.globl vm_enter\n"
        "vm_enter:\n"
        "  addi sp, sp, -16\n"
        "  sd ra, 8(sp)\n"                      // on the stack, out of DOM1's reach
        "  mv t0, a0\n"
        "  mv a0, a1\n"
        "  mv a1, a2\n"
        "  .insn i 0x5b, 0, ra, t0, 512\n"      // domcall t0, DOM1, label 0
        "  ld ra, 8(sp)\n"                      // back from its domret, in DOM0
        "  addi sp, sp, 16\n"
        "  ret\n"
        // tramp_prim: method -> vm_primitive(a0, a1) -> method. Label 1
        // reaches this pad and nothing else in the VM
        ".balign 4\n.globl tramp_prim\n"
        "tramp_prim:\n"
        "  .insn i 0x5b, 2, x0, x0, 1\n"        // domlpad DOM0, label 1
        "  call vm_primitive\n"
        "  .insn i 0x5b, 1, x0, x0, 0\n"        // domret, into the method
        // tramp_exit: method -> the executive, one way (label 2)
        ".balign 4\n.globl tramp_exit\n"
        "tramp_exit:\n"
        "  .insn i 0x5b, 2, x0, x0, 2\n"        // domlpad DOM0, label 2
        "  call vm_exit\n"
        "  unimp\n"                             // vm_exit never returns
        ".option pop\n");
uint64_t vm_enter(const uint32_t *method, uint64_t a0, uint64_t a1);
extern char tramp_prim[], tramp_exit[];

// Generated code: a few RV64 encodings
enum { ZERO = 0, RA = 1, T0 = 5, A0 = 10, A1 = 11, A7 = 17 };

static uint32_t itype(unsigned op, unsigned f3, unsigned rd, unsigned rs1, int imm)
{
  return ((uint32_t)imm & 0xfff) << 20 | rs1 << 15 | f3 << 12 | rd << 7 | op;
}

static uint32_t stype(unsigned op, unsigned f3, unsigned rs1, unsigned rs2, int imm)
{
  return ((uint32_t)imm >> 5 & 0x7f) << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 |
         ((uint32_t)imm & 0x1f) << 7 | op;
}

#define PAD(dom, label) itype(0x5b, 2, ZERO, ZERO, (dom) << 9 | (label))
#define DOMCALL(rs1, dom, label) itype(0x5b, 0, RA, rs1, (dom) << 9 | (label))
#define DOMRET itype(0x5b, 1, ZERO, ZERO, 0)
#define LD(rd, rs1, off) itype(0x03, 3, rd, rs1, off)
#define SD(rs2, rs1, off) stype(0x23, 3, rs1, rs2, off)
#define ADDI(rd, rs1, imm) itype(0x13, 0, rd, rs1, imm)
#define AUIPC(rd) ((uint32_t)(rd) << 7 | 0x17)
#define ECALL 0x00000073u
#define UNIMP 0xc0001073u

static uint32_t *emit_p;

static void emit(uint32_t insn)
{
  *emit_p++ = insn;
}

// rd = addr, for the VM's addresses, which are below 2 GiB
static void emit_li(unsigned rd, uintptr_t addr)
{
  emit((uint32_t)((addr + 0x800) >> 12 & 0xfffff) << 12 | rd << 7 | 0x37);
  emit(ADDI(rd, rd, (int)(addr & 0xfff) << 20 >> 20));
}

// The methods; a0 and a1 are their arguments, a0 their result
static uint32_t *jit;
static uint32_t *m_inc;       // ++obj[0]
static uint32_t *m_inc_step;  //   its addi, which the VM patches
static uint32_t *m_prim;      // primitive a1 on a0, plus 100
static uint32_t *m_wrong;     // primitive through the exit's label
static uint32_t *m_exit;      // exit to the executive with a0
static uint32_t *m_selfpatch; // a store to its own code
static uint32_t *m_sys;       // a syscall, then a0 + 1

static uint32_t *method_start(void)
{
  emit_p = (uint32_t *)(((uintptr_t)emit_p + 63) & ~(uintptr_t)63);
  emit(PAD(1, 0));
  return emit_p - 1;
}

static void generate(void)
{
  emit_p = jit;

  m_inc = method_start();
  emit(LD(T0, A0, 0));
  m_inc_step = emit_p;
  emit(ADDI(T0, T0, 1));
  emit(SD(T0, A0, 0));
  emit(ADDI(A0, T0, 0));
  emit(DOMRET);

  m_prim = method_start();
  emit_li(T0, (uintptr_t)tramp_prim);
  emit(DOMCALL(T0, 0, 1));
  emit(ADDI(A0, A0, 100));
  emit(DOMRET);

  m_wrong = method_start();
  emit_li(T0, (uintptr_t)tramp_prim);
  emit(DOMCALL(T0, 0, 2));
  emit(DOMRET);

  m_exit = method_start();
  emit_li(T0, (uintptr_t)tramp_exit);
  emit(DOMCALL(T0, 0, 2));
  emit(UNIMP);

  m_selfpatch = method_start();
  emit(AUIPC(T0));
  emit(SD(ZERO, T0, 0));
  emit(DOMRET);

  m_sys = method_start();
  emit(ADDI(A7, ZERO, 172));
  emit(ECALL);
  emit(ADDI(A0, A0, 1));
  emit(DOMRET);

  __builtin___clear_cache((char *)jit, (char *)emit_p);
}

// The executive. Entries nest (a primitive may run a method), each as a
// level with a base, dsp when it started, and a return point for exits. The
// bases are on key 0.
#define MAX_LEVELS 8
static __thread uint64_t bases[MAX_LEVELS];
static __thread sigjmp_buf *levels[MAX_LEVELS];
static __thread int nlevels;
static __thread uint64_t exit_value;

static void set_udsp(uint64_t v)
{
  __asm__ volatile("csrw " CSR_STR(CSR_UDSP) ", %0" :: "r"(v) : "memory");
}

// Runs method(a0, a1) as a new level; its result, or its exit value
static uint64_t vm_run(const uint32_t *method, uint64_t a0, uint64_t a1)
{
  sigjmp_buf jb;
  volatile uint64_t r;
  int level = nlevels++;

  // Records below the base are this level's, above it the outer levels'
  bases[level] = csr_read(CSR_UDSP);
  levels[level] = &jb;
  if (!sigsetjmp(jb, 0)) {
    r = vm_enter(method, a0, a1);
  } else {
    // An exit skipped the domrets of this level's records: drop them, but
    // not the outer levels', which are still needed
    set_udsp(bases[level]);
    r = exit_value;
  }
  nlevels = level;
  return r;
}

// From tramp_exit, in DOM0: back to the innermost level's return point
__attribute__((used, noreturn)) void vm_exit(uint64_t v)
{
  exit_value = v;
  siglongjmp(*levels[nlevels - 1], 1);
}

// The top of the main thread's DRS: its base when no level runs
static uintptr_t top;

// After a fault ended a run past every level: drop them, empty the DRS
static void vm_recover(void)
{
  nlevels = 0;
  set_udsp(top);
}

// Primitives
enum { OP_READ, OP_CALLBACK, OP_UNWIND, OP_BAD_RESET };

static volatile uint64_t vm_secret = 0x5ec7e7;
static volatile uint64_t cb_base, cb_after;

static uint64_t typed_ld(uint64_t p)
{
  uint64_t v;
  __asm__ volatile(".insn i 0x0b, 3, %0, 0(%1)" : "=r"(v) : "r"(p) : "memory");
  return v;
}

// From tramp_prim, in DOM0: primitive op on a0. cb_base and cb_after record
// dsp around a nested level, which must leave it as it found it.
__attribute__((used)) uint64_t vm_primitive(uint64_t a0, uint64_t op)
{
  uint64_t r, mine;

  switch (op) {
  case OP_READ:
    // a0 comes from the heap: DOM1's rights, through dtarget[0]
    return typed_ld(a0);
  case OP_CALLBACK:     // a nested method that returns
    cb_base = csr_read(CSR_UDSP);
    r = vm_run(m_inc, a0, 0);
    cb_after = csr_read(CSR_UDSP);
    return r;
  case OP_UNWIND:       // one that exits
    cb_base = csr_read(CSR_UDSP);
    r = vm_run(m_exit, a0, 0);
    cb_after = csr_read(CSR_UDSP);
    return r;
  case OP_BAD_RESET:
    // A reset past the innermost base discards our caller's records;
    // moving back down is refused (DSPRANGE)
    mine = csr_read(CSR_UDSP);
    set_udsp(bases[0]);
    set_udsp(mine);
    return 0;
  }
  return ~0ULL;
}

// A signal on the alternate stack, interrupting a method
static uint8_t altstack[64 * 1024] __attribute__((aligned(16)));
static volatile uintptr_t handler_sp;
static volatile uint64_t handler_dom;

static void sigsys_handler(int sig, siginfo_t *si, void *uc)
{
  volatile int here;

  (void)sig, (void)si, (void)uc;
  handler_sp = (uintptr_t)&here;
  handler_dom = csr_read(CSR_CURDOM);
}

// A thread the VM did not create
static volatile uint64_t *obj;
static volatile uint64_t thread_udsp0, thread_r, thread_after, thread_top;

static void *foreign_thread(void *arg)
{
  (void)arg;
  thread_udsp0 = csr_read(CSR_UDSP);
  if (thread_udsp0 == 0 && jd_prctl(PR_JITDOMAIN_DRS_ALLOC, 0, 0, 0) == 0)
    thread_top = csr_read(CSR_UDSP);
  thread_r = vm_run(m_inc, (uint64_t)obj, 0);
  thread_after = csr_read(CSR_UDSP);
  return NULL;
}

int main(void)
{
  struct jitdomain_matrices m;
  struct sigaction sa;
  stack_t ss;
  pthread_t th;
  volatile uint32_t saved;
  long page;

  user_init();
  page = sysconf(_SC_PAGESIZE);

  CHECK_EQ(jd_enable(), 0);
  CHECK_EQ(jd_prctl(PR_JITDOMAIN_DRS_ALLOC, 0, 0, 0), 0);
  top = csr_read(CSR_UDSP);
  CHECK_EQ(jd_pkey_alloc(0, 0), 1);
  CHECK_EQ(jd_pkey_alloc(0, 0), 2);
  jit = mmap(NULL, page, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  obj = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK_EQ(jit == MAP_FAILED || obj == MAP_FAILED, 0);
  CHECK_EQ((uintptr_t)tramp_exit < 0x7ffff800, 1);
  CHECK_EQ(jd_pkey_mprotect(jit, page, PROT_READ | PROT_WRITE | PROT_EXEC, 1), 0);
  CHECK_EQ(jd_pkey_mprotect(obj, page, PROT_READ | PROT_WRITE, 2), 0);

  // DOM1: X on the code, RW on the heap, nothing on key 0. DOM0's typed
  // accesses get DOM1's rights
  m = permissive();
  set_rights(&m, 1, 1, DR_X);
  set_rights(&m, 1, 2, DR_R | DR_W);
  m.dtarget = (m.dtarget & ~7ULL) | 1;
  m.dgate = GATE(0, 1) | GATE(1, 0);
  CHECK_EQ(jd_set(&m), 0);
  generate();
  obj[0] = 41;

  // A method, and the VM's data out of its reach
  CHECK_EQ(vm_run(m_inc, (uint64_t)obj, 0), 42);
  CHECK_EQ(obj[0], 42);
  CHECK_EQ(csr_read(CSR_CURDOM), 0);
  CHECK_EQ(csr_read(CSR_UDSP), top);
  CHECK_EQ(TRY(vm_run(m_inc, (uint64_t)&vm_secret, 0)), SIGSEGV);
  CHECK_EQ(last_fault.code, SEGV_PKUERR);
  CHECK_EQ(last_fault.addr, (uintptr_t)&vm_secret);
  CHECK_EQ(vm_secret, 0x5ec7e7);
  vm_recover();

  // A primitive through the trampoline, following a heap pointer, and a
  // forged pointer to VM data
  CHECK_EQ(vm_run(m_prim, (uint64_t)obj, OP_READ), 142);
  CHECK_EQ(csr_read(CSR_UDSP), top);
  CHECK_EQ(TRY(vm_run(m_prim, (uint64_t)&vm_secret, OP_READ)), SIGSEGV);
  CHECK_EQ(last_fault.code, SEGV_PKUERR);
  CHECK_EQ(last_fault.addr, (uintptr_t)&vm_secret);
  vm_recover();

  // The wrong label for the trampoline
  CHECK_EQ(TRY(vm_run(m_wrong, 0, 0)), SIGSEGV);
  CHECK_EQ(last_fault.code, SEGV_CPERR);
  CHECK_EQ(last_fault.addr, (uintptr_t)tramp_prim);
  vm_recover();

  // An exit to the executive discards the method's records
  CHECK_EQ(vm_run(m_exit, 9, 0), 9);
  CHECK_EQ(csr_read(CSR_CURDOM), 0);
  CHECK_EQ(csr_read(CSR_UDSP), top);

  // A callback: a nested entry, on its own base
  CHECK_EQ(vm_run(m_prim, (uint64_t)obj, OP_CALLBACK), 143);
  CHECK_EQ(obj[0], 43);
  CHECK_EQ(cb_base, top - 16);
  CHECK_EQ(cb_after, top - 16);
  CHECK_EQ(csr_read(CSR_UDSP), top);

  // An exit from the callback resets to its base only: the outer method
  // resumes and returns
  CHECK_EQ(vm_run(m_prim, 7, OP_UNWIND), 107);
  CHECK_EQ(cb_after, top - 16);
  CHECK_EQ(csr_read(CSR_UDSP), top);

  // A reset past the innermost base, caught at the restore
  CHECK_EQ(TRY(vm_run(m_prim, (uint64_t)obj, OP_BAD_RESET)), SIGSEGV);
  CHECK_EQ(last_fault.code, SEGV_CPERR);
  CHECK_EQ(csr_read(CSR_UDSP), top);
  vm_recover();

  // Patching: by the VM, then by the code itself
  saved = *m_inc_step;
  *m_inc_step = ADDI(T0, T0, 5);
  __builtin___clear_cache((char *)m_inc_step, (char *)(m_inc_step + 1));
  CHECK_EQ(vm_run(m_inc, (uint64_t)obj, 0), 48);
  *m_inc_step = saved;
  __builtin___clear_cache((char *)m_inc_step, (char *)(m_inc_step + 1));
  CHECK_EQ(vm_run(m_inc, (uint64_t)obj, 0), 49);
  saved = m_selfpatch[1];
  CHECK_EQ(TRY(vm_run(m_selfpatch, 0, 0)), SIGSEGV);
  CHECK_EQ(last_fault.code, SEGV_PKUERR);
  CHECK_EQ(last_fault.addr, (uintptr_t)(m_selfpatch + 1));
  CHECK_EQ(m_selfpatch[1], saved);
  vm_recover();

  // A signal on the alternate stack, returning into the method
  ss.ss_sp = altstack;
  ss.ss_size = sizeof(altstack);
  ss.ss_flags = 0;
  CHECK_EQ(sigaltstack(&ss, NULL), 0);
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = sigsys_handler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigaction(SIGSYS, &sa, NULL);
  CHECK_EQ(vm_run(m_sys, 5, 0), 6);
  CHECK_EQ(handler_sp >= (uintptr_t)altstack && handler_sp < (uintptr_t)altstack + sizeof(altstack), 1);
  CHECK_EQ(handler_dom, 0);
  CHECK_EQ(csr_read(CSR_UDSP), top);

  // A thread without a DRS
  CHECK_EQ(pthread_create(&th, NULL, foreign_thread, NULL), 0);
  CHECK_EQ(pthread_join(th, NULL), 0);
  CHECK_EQ(thread_udsp0, 0);
  CHECK_EQ(thread_top != 0, 1);
  CHECK_EQ(thread_r, 50);
  CHECK_EQ(thread_after, thread_top);

  return user_done("u-jit");
}
