// u-fault: what a fault tells the handler. A domain access fault is
// SIGSEGV/SEGV_PKUERR at the faulting address, with si_pkey the key of the
// page; a domain CFI fault is SIGSEGV/SEGV_CPERR at the pc of the faulting
// instruction. With EN = 1, every signal frame carries one JDSG extension
// record after the FP state, then the END header: the interrupted context's
// curdom (dpend included) and dsp, and the subcode of the CFI fault being
// delivered, 0 for any other signal. Without EN, the list is empty.

#include <ucontext.h>
#include "user.h"

#define GATE(from, to) (1ULL << (8 * (from) + (to)))
#define JDSG_MAGIC 0x4A445347u
#define PEND_VALUE(dom, label) ((uint64_t)(0x1000 | ((label) << 3) | (dom)) << 3)

struct ctx_hdr { uint32_t magic, size; };
struct jd_state { uint64_t curdom, dsp, dcause; };

static uintptr_t top;

static volatile struct {
  int sig, code;
  uintptr_t addr;
  unsigned pkey;
  int records, jd, ended;   // extension records seen, JDSG ones, END seen
  uint32_t reserved;
  struct jd_state st;
} f;

// Walks the extension records, which start in the last 8 bytes of the
// 528-byte FP state
static void handler(int sig, siginfo_t *si, void *ucv)
{
  ucontext_t *uc = ucv;
  const uint8_t *fp = (const uint8_t *)&uc->uc_mcontext.__fpregs;
  const struct ctx_hdr *h = (const void *)(fp + 520);

  f.sig = sig;
  f.code = si->si_code;
  f.addr = (uintptr_t)si->si_addr;
  f.pkey = si->si_pkey;
  f.reserved = *(const uint32_t *)(fp + 516);
  for (int i = 0; i < 8; i++) {
    if (h->magic == 0) {
      f.ended = h->size == 0;
      break;
    }
    f.records++;
    if (h->magic == JDSG_MAGIC && h->size == sizeof(*h) + sizeof(struct jd_state)) {
      f.jd++;
      memcpy((void *)&f.st, h + 1, sizeof(f.st));
    }
    h = (const void *)((const uint8_t *)h + h->size);
  }
  last_fault.sig = sig;
  fault_return(sig);
}

static int fault(void (*fn)(void))
{
  memset((void *)&f, 0, sizeof(f));
  int sig = TRY(fn());
  if (top && csr_read(CSR_UDSP) != top)   // udsp is illegal before ENABLE
    __asm__ volatile("csrw " CSR_STR(CSR_UDSP) ", %0" :: "r"(top) : "memory");
  return sig;
}

// The faults, each in a function so that its address is known
static uint64_t *p, *q;
static uint32_t *code;
static uintptr_t pc_at;

static volatile uintptr_t null_addr = 16;
static void null_load(void) { (void)*(volatile uint64_t *)null_addr; }
static void dom0_load_key2(void) { (void)*(volatile uint64_t *)q; }
static void dom0_fetch_key2(void) { ((void (*)(void))code)(); }

// In DOM3, after its pad: insn. pc_at gets the address of insn.
#define IN_DOM3(insn, ...) \
  __asm__ volatile( \
      "lla t1, 3f\n" \
      "sd t1, %[at]\n" \
      "lla t0, 1f\n" \
      ".insn i 0x5b, 0, ra, t0, %[imm]\n" \
      "j 2f\n" \
      ".balign 4\n" \
      "1: .insn i 0x5b, 2, x0, x0, %[imm]\n" \
      "3: " insn "\n" \
      ".insn i 0x5b, 1, x0, x0, 0\n" \
      "2:\n" \
      : [at] "=m"(pc_at) : [imm] "i"(DOM_IMM(3, 0)), ##__VA_ARGS__ \
      : "t0", "t1", "a0", "a7", "ra", "memory")

static void dom3_load_key2(void) { IN_DOM3("ld a0, 0(%[p])", [p] "r"(q)); }
static void dom3_store_key1(void) { IN_DOM3("sd x0, 0(%[p])", [p] "r"(p)); }
static void dom3_read_udsp(void) { IN_DOM3("csrr a0, " CSR_STR(CSR_UDSP)); }
static void dom3_getpid(void) { IN_DOM3("li a7, 172\n ecall"); }

// domcall into DOM3 at a plain instruction instead of a pad
static void dom3_no_pad(void)
{
  __asm__ volatile(
      "lla t0, 1f\n"
      "sd t0, %[at]\n"
      ".insn i 0x5b, 0, ra, t0, %[imm]\n"
      "j 2f\n"
      ".balign 4\n"
      "1: nop\n"
      "2:\n"
      : [at] "=m"(pc_at) : [imm] "i"(DOM_IMM(3, 0)) : "t0", "ra", "memory");
}

// domcall into DOM5, whose gate is closed
static void dom5_closed_gate(void)
{
  __asm__ volatile(
      "lla t0, 1f\n"
      "lla t1, 3f\n"
      "sd t1, %[at]\n"
      "3: .insn i 0x5b, 0, ra, t0, %[imm]\n"
      "j 2f\n"
      ".balign 4\n"
      "1: .insn i 0x5b, 2, x0, x0, %[imm]\n"
      "2:\n"
      : [at] "=m"(pc_at) : [imm] "i"(DOM_IMM(5, 0)) : "t0", "t1", "ra", "memory");
}

#define CHECK_RECORD(dom_value, dsp_value, dcause_value) do { \
    CHECK_EQ(f.records, 1); \
    CHECK_EQ(f.jd, 1); \
    CHECK_EQ(f.ended, 1); \
    CHECK_EQ(f.reserved, 0); \
    CHECK_EQ(f.st.curdom, dom_value); \
    CHECK_EQ(f.st.dsp, dsp_value); \
    CHECK_EQ(f.st.dcause, dcause_value); \
  } while (0)

int main(void)
{
  struct jitdomain_matrices m;
  struct sigaction sa;
  long page;

  user_init();
  page = sysconf(_SC_PAGESIZE);
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = handler;
  sa.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGSYS, &sa, NULL);

  // Without EN: an ordinary fault, an empty record list
  CHECK_EQ(fault(null_load), SIGSEGV);
  CHECK_EQ(f.code, SEGV_MAPERR);
  CHECK_EQ(f.addr, 16);
  CHECK_EQ(f.records, 0);
  CHECK_EQ(f.ended, 1);
  CHECK_EQ(f.reserved, 0);

  CHECK_EQ(jd_enable(), 0);
  CHECK_EQ(jd_prctl(PR_JITDOMAIN_DRS_ALLOC, 0, 0, 0), 0);
  top = csr_read(CSR_UDSP);
  CHECK_EQ(jd_pkey_alloc(0, 0), 1);
  CHECK_EQ(jd_pkey_alloc(0, 0), 2);
  p = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  q = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  code = mmap(NULL, page, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK_EQ(p == MAP_FAILED || q == MAP_FAILED || code == MAP_FAILED, 0);
  code[0] = 0x00008067;   // ret
  __builtin___clear_cache((char *)code, (char *)(code + 1));
  CHECK_EQ(jd_pkey_mprotect(p, page, PROT_READ | PROT_WRITE, 1), 0);
  CHECK_EQ(jd_pkey_mprotect(q, page, PROT_READ | PROT_WRITE, 2), 0);
  CHECK_EQ(jd_pkey_mprotect(code, page, PROT_READ | PROT_EXEC, 2), 0);

  // DOM0: nothing on key 2. DOM3: X on key 0, R on key 1, nothing else
  m = permissive();
  set_rights(&m, 0, 2, 0);
  set_rights(&m, 3, 0, DR_X);
  set_rights(&m, 3, 1, DR_R);
  m.dgate = GATE(0, 3);
  CHECK_EQ(jd_set(&m), 0);

  // Ordinary faults get the record too, with dcause 0
  CHECK_EQ(fault(null_load), SIGSEGV);
  CHECK_EQ(f.code, SEGV_MAPERR);
  CHECK_RECORD(0, top, 0);

  // CFI faults: si_addr is the pc, the record has the subcode
  CHECK_EQ(fault(dom5_closed_gate), SIGSEGV);
  CHECK_EQ(f.code, SEGV_CPERR);
  CHECK_EQ(f.addr, pc_at);
  CHECK_RECORD(0, top, DCAUSE_GATE);

  CHECK_EQ(fault(dom3_no_pad), SIGSEGV);
  CHECK_EQ(f.code, SEGV_CPERR);
  CHECK_EQ(f.addr, pc_at);
  CHECK_RECORD(PEND_VALUE(3, 0), top - 8, DCAUSE_LPAD);   // entry pending

  CHECK_EQ(fault(dom3_read_udsp), SIGSEGV);
  CHECK_EQ(f.code, SEGV_CPERR);
  CHECK_EQ(f.addr, pc_at);
  CHECK_RECORD(3, top - 8, DCAUSE_DSP);

  // Access faults: si_pkey is the page's key; dcause does not keep the
  // last CFI subcode
  CHECK_EQ(fault(dom0_load_key2), SIGSEGV);
  CHECK_EQ(f.code, SEGV_PKUERR);
  CHECK_EQ(f.addr, q);
  CHECK_EQ(f.pkey, 2);
  CHECK_RECORD(0, top, 0);

  CHECK_EQ(fault(dom0_fetch_key2), SIGSEGV);
  CHECK_EQ(f.code, SEGV_PKUERR);
  CHECK_EQ(f.addr, code);
  CHECK_EQ(f.pkey, 2);

  CHECK_EQ(fault(dom3_load_key2), SIGSEGV);
  CHECK_EQ(f.code, SEGV_PKUERR);
  CHECK_EQ(f.addr, q);
  CHECK_EQ(f.pkey, 2);
  CHECK_RECORD(3, top - 8, 0);

  CHECK_EQ(fault(dom3_store_key1), SIGSEGV);
  CHECK_EQ(f.code, SEGV_PKUERR);
  CHECK_EQ(f.addr, p);
  CHECK_EQ(f.pkey, 1);
  CHECK_RECORD(3, top - 8, 0);

  // Other signals: SIGSYS from DOM3
  CHECK_EQ(fault(dom3_getpid), SIGSYS);
  CHECK_RECORD(3, top - 8, 0);

  return user_done("u-fault");
}
