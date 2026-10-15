// Test stubs, run in S or U mode by run_lower(). Each one takes its operands
// in a0/a1, does one thing, and ends with an ecall. Stubs are position
// independent, so tests can copy [name, name_end) into a page with any key.

#include "jitdomain.h"

#define STUB(name, ...) \
  .globl name, name##_end; .align 2; name: __VA_ARGS__; ecall; name##_end:

  .text

  .globl stub_ecall, stub_ecall_end
  .align 2
stub_ecall:
  ecall
stub_ecall_end:

// Ordinary accesses to a0; stores and AMOs write a1
STUB(stub_lb,  lb  a0, 0(a0))
STUB(stub_lh,  lh  a0, 0(a0))
STUB(stub_lw,  lw  a0, 0(a0))
STUB(stub_ld,  ld  a0, 0(a0))
STUB(stub_lbu, lbu a0, 0(a0))
STUB(stub_lhu, lhu a0, 0(a0))
STUB(stub_lwu, lwu a0, 0(a0))
STUB(stub_sb,  sb  a1, 0(a0))
STUB(stub_sh,  sh  a1, 0(a0))
STUB(stub_sw,  sw  a1, 0(a0))
STUB(stub_sd,  sd  a1, 0(a0))
STUB(stub_amoadd_d, amoadd.d a0, a1, (a0))
STUB(stub_lr_d, lr.d a0, (a0))
STUB(stub_lr_sc_d, lr.d t0, (a0); sc.d a0, a1, (a0))

// Typed accesses
STUB(stub_dlb,  dlb  a0, 0(a0))
STUB(stub_dlh,  dlh  a0, 0(a0))
STUB(stub_dlw,  dlw  a0, 0(a0))
STUB(stub_dld,  dld  a0, 0(a0))
STUB(stub_dlbu, dlbu a0, 0(a0))
STUB(stub_dlhu, dlhu a0, 0(a0))
STUB(stub_dlwu, dlwu a0, 0(a0))
STUB(stub_dsb,  dsb  a1, 0(a0))
STUB(stub_dsh,  dsh  a1, 0(a0))
STUB(stub_dsw,  dsw  a1, 0(a0))
STUB(stub_dsd,  dsd  a1, 0(a0))
STUB(stub_dlbu_m2048, dlbu a0, -2048(a0))
STUB(stub_dlbu_p2047, dlbu a0, 2047(a0))
STUB(stub_dsb_m2048,  dsb  a1, -2048(a0))
STUB(stub_dsb_p2047,  dsb  a1, 2047(a0))
STUB(stub_dl_rsvd, .insn i 0x0b, 7, a0, 0(a0))
STUB(stub_ds_rsvd, .insn s 0x2b, 7, a1, 0(a0))

// CSR accesses: stub_csrr_<addr> reads into a0, stub_csrw_<addr> writes a1
  .irp csr, 0x5c0,0x5c1,0x5c2,0x5c3,0x5c4,0x5c5,0x5c6,0xdc0,0xcc0,0x7c3,0x7c4,0x7c5
  .globl stub_csrr_\csr, stub_csrw_\csr
  .align 2
stub_csrr_\csr:
  csrr a0, \csr
  ecall
  .align 2
stub_csrw_\csr:
  csrw \csr, a1
  ecall
  .endr
