// T-LPAD stubs and snippets. Callers run from the image; the snippets are
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

// domcall to a0 with the link in ra
STUB(call_0_5, domcall ra, a0, 0, 5)
STUB(call_1_5, domcall ra, a0, 1, 5)
STUB(call_1_0, domcall ra, a0, 1, 0)

// An ordinary indirect jump to a0
STUB(jalr_a0, jalr ra, 0(a0))

// Pads and non-pads: record curdom in s2, then end
SNIP(pad_0_5, domlpad 0, 5; csrr s2, CSR_CURDOM; ecall)
SNIP(pad_1_5, domlpad 1, 5; csrr s2, CSR_CURDOM; ecall)
SNIP(pad_1_0, domlpad 1, 0; csrr s2, CSR_CURDOM; ecall)
SNIP(pad_1_6, domlpad 1, 6; csrr s2, CSR_CURDOM; ecall)
SNIP(pad_2_5, domlpad 2, 5; csrr s2, CSR_CURDOM; ecall)
SNIP(nopad,   csrr s2, CSR_CURDOM; ecall)

// A pad 2 bytes into the snippet, behind a c.nop
SNIP(pad_1_5_at2, .2byte 0x0001; domlpad 1, 5; csrr s2, CSR_CURDOM; ecall)

// A jump and a branch 0x500 bytes forward, which crosses into the next page
// when the snippet is placed at offset 0xc00
SNIP(jal_fwd, j .+0x500)
SNIP(beq_fwd, beq zero, zero, .+0x500)

  .option pop
