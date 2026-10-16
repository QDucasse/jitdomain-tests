// T-TRAP stubs and snippets. Callers and the S-mode handler run from the
// image; the snippets are copied into test pages, so they are position
// independent and use no compressed instructions.

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

// domcall to a0, keeping a1 for the callee
STUB(call_1_5, domcall ra, a0, 1, 5)

// Callees: enter DOM1, then end with an ecall, an ebreak, or a load from a1.
// Each records curdom in s2 just before it ends.
SNIP(pad_ecall,  domlpad 1, 5; csrr s2, CSR_CURDOM; ecall)
SNIP(pad_ebreak, domlpad 1, 5; csrr s2, CSR_CURDOM; ebreak)
SNIP(pad_load,   domlpad 1, 5; ld a0, 0(a1); csrr s2, CSR_CURDOM; ecall)
SNIP(nopad,      csrr s2, CSR_CURDOM; ecall)

// S-mode trap handler (stvec): record dstatus in s4, curdom in s5, scause in
// s7, stval in s8 and dcause in s9, then end with an ecall to M
STUB(s_handler, csrr s4, CSR_DSTATUS; csrr s5, CSR_CURDOM; csrr s7, scause;
     csrr s8, stval; csrr s9, CSR_DCAUSE)

// S mode: sret to a0 in U mode, with dstatus = a1
STUB(s_sret, csrw sepc, a0; csrw CSR_DSTATUS, a1; li t0, 0x100; csrc sstatus, t0; sret)

  .option pop
