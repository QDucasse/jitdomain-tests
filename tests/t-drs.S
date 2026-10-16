// T-DRS stubs and snippets. Callers run from the image; the snippets are
// copied into test pages, so they are position independent and use no
// compressed instructions apart from the explicit c.nop.

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

// domcall to a0, then record curdom in s2 once back
STUB(call_1_5, domcall ra, a0, 1, 5; csrr s2, CSR_CURDOM)
STUB(call_nest, li s6, 0; domcall ra, a0, 1, 0; csrr s2, CSR_CURDOM)

// domret on its own
STUB(ret_only, domret)

// The callee: record curdom in s3, then return
SNIP(callee, domlpad 1, 5; csrr s3, CSR_CURDOM; domret)

// Nested entries: a1 counts the domcalls still to make, s6 the entries made.
// a0 holds the snippet's own address.
SNIP(nest,
  domlpad 1, 0;
  addi s6, s6, 1;
  addi a1, a1, -1;
  beqz a1, 1f;
  domcall ra, a0, 1, 0;
1:
  domret)

// A domret target: record curdom in s2, then end. The second one starts
// 2 bytes in.
SNIP(land, csrr s2, CSR_CURDOM; ecall)
SNIP(land_at2, .2byte 0x0001; csrr s2, CSR_CURDOM; ecall)

  .option pop
