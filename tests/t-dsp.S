// T-DSP stubs. Every udsp access writes the old value to a1, so that the
// test can check that a faulting one leaves rd alone. The callee is copied
// into page 1, so it is position independent.

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

// Reads: rs1 = x0 or a zero immediate, so none of them writes
STUB(udsp_r,    csrr   a1, CSR_UDSP)
STUB(udsp_rc0,  csrrc  a1, CSR_UDSP, x0)
STUB(udsp_rsi0, csrrsi a1, CSR_UDSP, 0)
STUB(udsp_rci0, csrrci a1, CSR_UDSP, 0)

// Writes, from a0 or from the immediate 8
STUB(udsp_rw,  csrrw  a1, CSR_UDSP, a0)
STUB(udsp_rs,  csrrs  a1, CSR_UDSP, a0)
STUB(udsp_rc,  csrrc  a1, CSR_UDSP, a0)
STUB(udsp_rwi, csrrwi a1, CSR_UDSP, 8)
STUB(udsp_rsi, csrrsi a1, CSR_UDSP, 8)
STUB(udsp_rci, csrrci a1, CSR_UDSP, 8)

// Run in S mode: dsp itself, unchecked
STUB(s_dsp_w, csrw CSR_DSP, a0)

// Reset udsp to a1, then domcall a0 and record curdom in s2 once back
STUB(reset_call, csrw CSR_UDSP, a1; domcall ra, a0, 1, 5; csrr s2, CSR_CURDOM)

// A nested entry into DOM0 (an FFI callback): save the current dsp as this
// entry's base in s4, reset to a1 (to that base when a1 = 0), domcall a0,
// then restore the base on exit
STUB(callback,
  csrr s4, CSR_UDSP;
  mv t1, s4;
  beqz a1, 1f;
  mv t1, a1;
1:
  csrw CSR_UDSP, t1;
  domcall ra, a0, 1, 5;
  .globl callback_restore;
callback_restore:
  csrw CSR_UDSP, s4)

// domret on its own
STUB(ret_only, domret)

// The callee: record curdom in s3, then return
SNIP(callee, domlpad 1, 5; csrr s3, CSR_CURDOM; domret)

// A domret target: record curdom in s2, then end
SNIP(land, csrr s2, CSR_CURDOM; ecall)

  .option pop
