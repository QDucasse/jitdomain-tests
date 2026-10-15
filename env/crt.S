// Entry, trap vector, and the switch from the M-mode monitor to a test stub
// running in S or U mode.

#include "jitdomain.h"

#define REGBYTES 8
#define FRAME (32 * REGBYTES)

  .section .text.init, "ax"
  .globl _start
_start:
  la sp, stack_top
  la t0, trap_stack_top
  csrw mscratch, t0
  la t0, trap_entry
  csrw mtvec, t0

  la t0, __bss_start
  la t1, __bss_end
1:
  bgeu t0, t1, 2f
  sd zero, 0(t0)
  addi t0, t0, 8
  j 1b
2:
  call main
  call exit

  .text

// uint64_t run_lower(void *entry, uint64_t mpp, uint64_t mdstatus, uint64_t a0, uint64_t a1)
//
// mret to entry at privilege mpp, with {curdom, dpend} restored from mdstatus.
// The stub runs until its first trap; the trap handler then resumes here and
// returns mcause (details in last_trap).
  .globl run_lower
  .align 2
run_lower:
  la t0, monitor_ctx
  sd ra, 0*REGBYTES(t0)
  sd sp, 1*REGBYTES(t0)
  sd s0, 2*REGBYTES(t0)
  sd s1, 3*REGBYTES(t0)
  sd s2, 4*REGBYTES(t0)
  sd s3, 5*REGBYTES(t0)
  sd s4, 6*REGBYTES(t0)
  sd s5, 7*REGBYTES(t0)
  sd s6, 8*REGBYTES(t0)
  sd s7, 9*REGBYTES(t0)
  sd s8, 10*REGBYTES(t0)
  sd s9, 11*REGBYTES(t0)
  sd s10, 12*REGBYTES(t0)
  sd s11, 13*REGBYTES(t0)

  csrw mepc, a0
  li t0, 3 << 11
  csrc mstatus, t0
  slli a1, a1, 11
  csrs mstatus, a1
  csrw CSR_MDSTATUS, a2
  mv a0, a3
  mv a1, a4
  // The stubs get a clean register file apart from a0/a1
  li sp, 0
  li ra, 0
  mret

// Every trap lands here. Traps from M mode are resumed past the faulting
// instruction (the suite is built without C); traps from S/U mode end the
// stub and return from run_lower.
  .align 2
trap_entry:
  csrrw sp, mscratch, sp
  addi sp, sp, -FRAME
  .irp n, 1,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
  sd x\n, \n*REGBYTES(sp)
  .endr
  csrr t0, mscratch
  sd t0, 2*REGBYTES(sp)

  mv a0, sp
  call handle_trap
  bnez a0, return_to_monitor

  .irp n, 1,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
  ld x\n, \n*REGBYTES(sp)
  .endr
  addi sp, sp, FRAME
  csrrw sp, mscratch, sp
  mret

return_to_monitor:
  addi sp, sp, FRAME
  csrw mscratch, sp
  la t0, monitor_ctx
  ld ra, 0*REGBYTES(t0)
  ld sp, 1*REGBYTES(t0)
  ld s0, 2*REGBYTES(t0)
  ld s1, 3*REGBYTES(t0)
  ld s2, 4*REGBYTES(t0)
  ld s3, 5*REGBYTES(t0)
  ld s4, 6*REGBYTES(t0)
  ld s5, 7*REGBYTES(t0)
  ld s6, 8*REGBYTES(t0)
  ld s7, 9*REGBYTES(t0)
  ld s8, 10*REGBYTES(t0)
  ld s9, 11*REGBYTES(t0)
  ld s10, 12*REGBYTES(t0)
  ld s11, 13*REGBYTES(t0)
  csrr a0, mcause
  ret

  .bss
  .align 3
monitor_ctx:
  .skip 14 * REGBYTES
