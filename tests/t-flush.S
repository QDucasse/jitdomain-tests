// T-FLUSH stubs and snippets. Callers and the S-mode handler run from the
// image; the snippets are copied into test pages, so they are position
// independent and use no compressed instructions. In each one the access
// under test comes right after the domain change, with nothing in between.

#include "jitdomain.h"

// A stub: does one thing, then ecall
#define STUB(name, ...) \
  .globl name, name##_end; .align 2; name: __VA_ARGS__; ecall; name##_end:

// A snippet copied by the test; ends however its body ends
#define SNIP(name, ...) \
  .globl name, name##_end; .align 2; name: __VA_ARGS__; name##_end:

  .text
  .option push
  .option norvc
  .option norelax

// domcall to a0
STUB(call_1_5, domcall ra, a0, 1, 5)

// domcall to a0, then an access to a1 right after the domret back
STUB(call_ld,  domcall ra, a0, 1, 5; ld a2, 0(a1))
STUB(call_sd,  domcall ra, a0, 1, 5; sd a1, 0(a1))
STUB(call_dld, domcall ra, a0, 1, 5; dld a2, 0(a1))

// Pads followed by an access to a1; each records curdom in s2 before ending
SNIP(pad_ld,  domlpad 1, 5; ld a2, 0(a1); csrr s2, CSR_CURDOM; ecall)
SNIP(pad_sd,  domlpad 1, 5; sd a1, 0(a1); csrr s2, CSR_CURDOM; ecall)
SNIP(pad_dld, domlpad 1, 5; dld a2, 0(a1); csrr s2, CSR_CURDOM; ecall)

// A callee that returns at once
SNIP(pad_ret, domlpad 1, 5; domret)

// Placed in the last word of a page, so that the next fetch is the first
// instruction of the next page
SNIP(pad_only,  domlpad 1, 5)
SNIP(call_only, domcall ra, a0, 1, 5)

// Placed at the start of a page: record curdom in s2, then end
SNIP(land, csrr s2, CSR_CURDOM; ecall)

// S-mode trap handler (stvec): a load from a1 as its first instruction
STUB(s_ld, ld a2, 0(a1); csrr s5, CSR_CURDOM)

// S mode: sret to a0 in U mode, with a1 pointing to {dstatus, the stub's a1}
STUB(s_sret,
  ld t1, 0(a1); ld a1, 8(a1);
  csrw sepc, a0; csrw CSR_DSTATUS, t1;
  li t0, 0x100; csrc sstatus, t0; sret)

  .option pop
