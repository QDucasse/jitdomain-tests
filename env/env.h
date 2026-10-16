// Bare-metal harness for the JITDomain conformance suite.
//
// Tests run as an M-mode monitor. They configure the domain state, then run
// short stubs (stubs.S) in S or U mode through run_lower(). A stub ends at its
// first trap: an ecall on success, or the fault under test.

#ifndef ENV_H
#define ENV_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "jitdomain.h"

#define PRV_U 0
#define PRV_S 1
#define PRV_M 3

#define CAUSE_ILLEGAL_INSN 2
#define CAUSE_ECALL_U 8
#define CAUSE_ECALL_S 9

#define PAGE_SIZE 4096
#define CSR_STR_(x) #x
#define CSR_STR(x) CSR_STR_(x)
#define csr_read(csr) ({ uint64_t __v; __asm__ volatile("csrr %0, " CSR_STR(csr) : "=r"(__v) :: "memory"); __v; })
#define csr_write(csr, v) __asm__ volatile("csrw " CSR_STR(csr) ", %0" :: "rK"((uint64_t)(v)) : "memory")
#define csr_set(csr, v) __asm__ volatile("csrs " CSR_STR(csr) ", %0" :: "rK"((uint64_t)(v)) : "memory")
#define csr_clear(csr, v) __asm__ volatile("csrc " CSR_STR(csr) ", %0" :: "rK"((uint64_t)(v)) : "memory")

#define fence_i() __asm__ volatile("fence.i" ::: "memory")

// The trap that ended the last stub, or the last trap taken in M mode
typedef struct {
  uint64_t cause, epc, tval, dcause, mdstatus;
  uint64_t a0;    // the stub's a0 at the trap
} trap_t;

extern volatile trap_t last_trap;
extern volatile uint64_t m_traps;   // traps taken (and skipped) in M mode

uint64_t run_lower(void *entry, uint64_t mpp, uint64_t mdstatus, uint64_t a0, uint64_t a1);

// Run a stub in U mode, in domain dom, with no pending entry
static inline uint64_t run_u(void *stub, int dom, uint64_t a0, uint64_t a1)
{
  return run_lower(stub, PRV_U, (uint64_t)dom << DSTATUS_XPD_SHIFT, a0, a1);
}

static inline uint64_t run_s(void *stub, int dom, uint64_t a0, uint64_t a1)
{
  return run_lower(stub, PRV_S, (uint64_t)dom << DSTATUS_XPD_SHIFT, a0, a1);
}

// CVA6 implements 8 PMP entries; every model runs with that many
#define NPMP 8

// Eight test pages, followed by the two DRS pages. dom_reset() tags page k with
// key k (and the DRS with key 7).
extern char pages[];
#define PAGE(k) ((uintptr_t)pages + (uintptr_t)(k) * PAGE_SIZE)

// Memory tagging, one implementation per key source (keysrc-*.c, KEYSRC= in the
// Makefile)
void keysrc_reset(void);
void keysrc_disable(void);   // what dstatus.EN = 0 requires of the tags
void key_set(int page, int key);
extern uint64_t keysrc_satp[2];   // satp for U and S stubs, read by run_lower()

// Domain configuration; the rights matrix is kept in a shadow copy and written
// with rights_apply()
void dom_reset(void);   // tags, dstatus = EN | DRSKEY=7, everything else at reset
void dom_disable(void); // dstatus.EN = 0, other fields kept
void rights_clear(void);
void rights_set(int e, int k, int r);
int rights_get(int e, int k);
void rights_apply(void);
uint64_t dtarget_identity(void);

// Copy a stub into memory that a test can tag with any key
void *place_stub(uintptr_t dst, const char *start, const char *end);

// Platform layer (platform/<name>/, PLATFORM= in the Makefile)
void platform_putchar(char c);
void exit(int code) __attribute__((noreturn));

// Reporting
int printf(const char *fmt, ...);
void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);

extern int checks, failures;
void check_failed(const char *file, int line, const char *expr, uint64_t got, uint64_t want);

#define CHECK_EQ(got, want) do { \
    uint64_t __g = (uint64_t)(got), __w = (uint64_t)(want); \
    checks++; \
    if (__g != __w) check_failed(__FILE__, __LINE__, #got, __g, __w); \
  } while (0)

// The stub ended with an ecall from mode prv
#define CHECK_OK(cause, prv) CHECK_EQ(cause, (prv) == PRV_U ? CAUSE_ECALL_U : CAUSE_ECALL_S)

// The stub ended with a domain access fault on addr
#define CHECK_DOM_FAULT(cause, addr) do { \
    CHECK_EQ(cause, CAUSE_DOMAIN_ACCESS); \
    CHECK_EQ(last_trap.tval, addr); \
  } while (0)

#define CHECK_CFI_FAULT(cause, sub) do { \
    CHECK_EQ(cause, CAUSE_DOMAIN_CFI); \
    CHECK_EQ(last_trap.dcause, sub); \
  } while (0)

int report(const char *name);

#define STUB(name) extern char name[], name##_end[]

#endif
