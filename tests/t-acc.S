// T-ACC stubs and snippets for the LR/SC and cache-block cases. The pad is
// copied into a test page, so it is position independent and uncompressed.

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

// LR on a1, then domcall to a0; the SC is done by the callee
STUB(lr_call, lr.d t0, (a1); domcall ra, a0, 1, 5)
SNIP(pad_sc, domlpad 1, 5; sc.d a0, a1, (a1); ecall)

// Zicbom and Zicboz on a0, as .insn so the toolchain needs neither extension
STUB(stub_cbo_inval, .insn i 0x0f, 2, x0, 0(a0))
STUB(stub_cbo_clean, .insn i 0x0f, 2, x0, 1(a0))
STUB(stub_cbo_flush, .insn i 0x0f, 2, x0, 2(a0))
STUB(stub_cbo_zero,  .insn i 0x0f, 2, x0, 4(a0))

  .option pop
