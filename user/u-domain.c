// u-domain: what the matrices do to the accesses of DOM1-7, entered with
// domcall. Each domain uses its own row: R on key 1 and W on key 2 give it
// loads on one and stores on the other, and nothing on key 0 data, whatever
// DOM0 may do. LR and AMOs need R and W. Typed accesses use dtarget[d], and
// fetches need X on the code's key. A fault in a domain runs the handler in
// DOM0, on the same DRS: the domcall's record is still there.
//
// The code run in a domain only uses registers, so that the domain needs
// nothing but X on key 0.

#include "user.h"

#define GATE(from, to) (1ULL << (8 * (from) + (to)))

enum { OP_LD, OP_SD, OP_AMO, OP_LR, OP_DLD, OP_DSD, OP_CALL };

static long page;
static uintptr_t top;
static struct jitdomain_matrices base;
static uint64_t global = 0x600d;

// domcall into DOMd, the access, domret. In: a1 = address, a2 = value.
// Out: a0.
#define IN_DOM(d, insn) \
  __asm__ volatile( \
      "lla t0, 1f\n" \
      ".insn i 0x5b, 0, ra, t0, %[imm]\n" \
      "j 2f\n" \
      ".balign 4\n" \
      "1: .insn i 0x5b, 2, x0, x0, %[imm]\n" \
      insn "\n" \
      ".insn i 0x5b, 1, x0, x0, 0\n" \
      "2:\n" \
      : "+r"(a0) : "r"(a1), "r"(a2), [imm] "i"(DOM_IMM(d, 0)) \
      : "t0", "ra", "memory")

#define OPS(d) \
  case d: \
    switch (op) { \
    case OP_LD:   IN_DOM(d, "ld a0, 0(a1)"); break; \
    case OP_SD:   IN_DOM(d, "sd a2, 0(a1)"); break; \
    case OP_AMO:  IN_DOM(d, "amoadd.d a0, a2, (a1)"); break; \
    case OP_LR:   IN_DOM(d, "lr.d a0, (a1)"); break; \
    case OP_DLD:  IN_DOM(d, ".insn i 0x0b, 3, a0, 0(a1)"); break; \
    case OP_DSD:  IN_DOM(d, ".insn s 0x2b, 3, a2, 0(a1)"); break; \
    case OP_CALL: IN_DOM(d, "jalr a1"); break; \
    } \
    break;

static __attribute__((noinline)) uint64_t run(unsigned d, unsigned op, const void *p, uint64_t v)
{
  register uint64_t a0 __asm__("a0") = 0;
  register const void *a1 __asm__("a1") = p;
  register uint64_t a2 __asm__("a2") = v;

  switch (d) {
    OPS(1) OPS(2) OPS(3) OPS(4) OPS(5) OPS(6) OPS(7)
  }
  return a0;
}

static void set_udsp(uint64_t v)
{
  __asm__ volatile("csrw " CSR_STR(CSR_UDSP) ", %0" :: "r"(v) : "memory");
}

// run() under TRY. After a fault, the handler must have run in DOM0 with
// the domcall's record still on the DRS, which is then dropped.
static int in_dom(unsigned d, unsigned op, const void *p, uint64_t v, uint64_t *out)
{
  int sig = TRY(*out = run(d, op, p, v));

  CHECK_EQ(csr_read(CSR_CURDOM) & CURDOM_DOM, 0);
  CHECK_EQ(csr_read(CSR_UDSP), sig ? top - 8 : top);
  if (sig)
    set_udsp(top);
  return sig;
}

#define CHECK_OK(d, op, p, v) do { \
    uint64_t __o; \
    CHECK_EQ(in_dom(d, op, p, v, &__o), 0); \
  } while (0)
#define CHECK_RES(d, op, p, v, want) do { \
    uint64_t __o = ~0ULL; \
    CHECK_EQ(in_dom(d, op, p, v, &__o), 0); \
    CHECK_EQ(__o, want); \
  } while (0)
#define CHECK_READ(d, op, p, want) CHECK_RES(d, op, p, 0, want)
#define CHECK_PKU(d, op, p, v) do { \
    uint64_t __o; \
    CHECK_EQ(in_dom(d, op, p, v, &__o), SIGSEGV); \
    CHECK_EQ(last_fault.code, SEGV_PKUERR); \
    CHECK_EQ(last_fault.addr, (uintptr_t)(p)); \
  } while (0)

static void apply(const struct jitdomain_matrices *m)
{
  CHECK_EQ(jd_set(m), 0);
}

int main(void)
{
  struct jitdomain_matrices m;
  uint64_t *p, *q;
  uint32_t *code;

  user_init();
  page = sysconf(_SC_PAGESIZE);
  CHECK_EQ(jd_enable(), 0);
  CHECK_EQ(jd_prctl(PR_JITDOMAIN_DRS_ALLOC, 0, 0, 0), 0);
  top = csr_read(CSR_UDSP);
  CHECK_EQ(jd_pkey_alloc(0, 0), 1);
  CHECK_EQ(jd_pkey_alloc(0, 0), 2);

  p = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  q = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  code = mmap(NULL, page, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK_EQ(p == MAP_FAILED || q == MAP_FAILED || code == MAP_FAILED, 0);
  code[0] = 0x02a00513;   // li a0, 42
  code[1] = 0x00008067;   // ret
  __builtin___clear_cache((char *)code, (char *)(code + 2));
  CHECK_EQ(jd_pkey_mprotect(p, page, PROT_READ | PROT_WRITE, 1), 0);
  CHECK_EQ(jd_pkey_mprotect(q, page, PROT_READ | PROT_WRITE, 2), 0);
  CHECK_EQ(jd_pkey_mprotect(code, page, PROT_READ | PROT_EXEC, 2), 0);

  // DOM0 keeps RWX everywhere; every other domain may enter and run its
  // code on key 0, nothing more
  base = permissive();
  for (unsigned d = 1; d < NDOMS; d++) {
    set_rights(&base, d, 0, DR_X);
    base.dgate |= GATE(0, d);
  }
  apply(&base);

  for (unsigned d = 1; d < NDOMS; d++) {
    int before = failures;

    *p = 0x100 + d;
    *q = 0x200 + d;

    // Its own row: R on key 1, W on key 2
    m = base;
    set_rights(&m, d, 1, DR_R);
    set_rights(&m, d, 2, DR_W);
    apply(&m);
    CHECK_READ(d, OP_LD, p, 0x100 + d);
    CHECK_PKU(d, OP_SD, p, 1);
    CHECK_PKU(d, OP_AMO, p, 1);
    CHECK_PKU(d, OP_LR, p, 0);
    CHECK_PKU(d, OP_LD, q, 0);
    CHECK_OK(d, OP_SD, q, 0x300 + d);
    CHECK_EQ(*q, 0x300 + d);
    CHECK_PKU(d, OP_AMO, q, 1);
    CHECK_EQ(*p, 0x100 + d);
    // Key 0 data: X is no data right
    CHECK_PKU(d, OP_LD, &global, 0);
    CHECK_PKU(d, OP_SD, &global, 0);
    CHECK_EQ(global, 0x600d);

    // RW: LR and AMOs
    set_rights(&m, d, 1, DR_R | DR_W);
    apply(&m);
    CHECK_RES(d, OP_AMO, p, 1, 0x100 + d);
    CHECK_READ(d, OP_LR, p, 0x101 + d);
    CHECK_EQ(*p, 0x101 + d);

    // Fetches: X on the code's key, which R and W do not give
    CHECK_PKU(d, OP_CALL, code, 0);
    set_rights(&m, d, 2, DR_X);
    apply(&m);
    CHECK_READ(d, OP_CALL, code, 42);

    // Typed accesses use dtarget[d]: DOM(d % 7 + 1), with R on key 2 only,
    // then DOM0
    {
      unsigned e = d % 7 + 1;
      m = base;
      set_rights(&m, e, 2, DR_R);
      m.dtarget = (dtarget_identity() & ~(7ULL << (3 * d))) | ((uint64_t)e << (3 * d));
      apply(&m);
      CHECK_READ(d, OP_DLD, q, 0x300 + d);
      CHECK_PKU(d, OP_DSD, q, 1);
      CHECK_PKU(d, OP_LD, q, 0);
      m.dtarget &= ~(7ULL << (3 * d));
      apply(&m);
      CHECK_OK(d, OP_DSD, q, 0x400 + d);
      CHECK_EQ(*q, 0x400 + d);
    }

    if (failures != before)
      printf("    (in DOM%u)\n", d);
  }

  return user_done("u-domain");
}
