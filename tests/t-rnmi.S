// T-RNMI helpers and RNMI handlers. Everything here runs in M mode.

#include "jitdomain.h"

#define CSR_MNEPC 0x741
#define CSR_MNSTATUS 0x744
#define MNSTATUS_NMIE 0x8
#define MNSTATUS_MNPP (3 << 11)
#define MSTATUS_MDT_BIT 42

// mnret, spelled out for toolchains without Smrnmi
#define MNRET .4byte 0x70200073

  .text
  .option push
  .option norvc
  .option norelax

// mnret to a pad, still in M mode, with mndstatus = a0. Returns the mndstatus
// read right after the pad in a0 and curdom in a1, after going back to DOM0
// through a second mnret.
  .globl m_mnret_pad
  .align 2
m_mnret_pad:
  la t0, 1f
  csrw CSR_MNEPC, t0
  li t1, MNSTATUS_MNPP
  csrs CSR_MNSTATUS, t1
  csrw CSR_MNDSTATUS, a0
  MNRET
  .align 2
1:
  domlpad 1, 5
  csrr a0, CSR_MNDSTATUS
  csrr a1, CSR_CURDOM
  la t0, 2f
  csrw CSR_MNEPC, t0
  csrs CSR_MNSTATUS, t1
  csrw CSR_MNDSTATUS, zero
  MNRET
2:
  ret

// Raise an RNMI with a domain entry pending. With mstatus.MDT set, mnret back
// to M mode restores mndstatus = a0 and lands on m_rnmi_nopad, which is not a
// pad. The LPAD fault is then a double trap, taken as an RNMI. rnmi_handler
// records what it sees in rnmi_seen and comes back at m_rnmi_back.
  .globl m_rnmi_pending, m_rnmi_nopad
  .align 2
m_rnmi_pending:
  la t0, m_rnmi_nopad
  csrw CSR_MNEPC, t0
  li t1, MNSTATUS_MNPP
  csrs CSR_MNSTATUS, t1
  csrw CSR_MNDSTATUS, a0
  li t0, 1
  slli t0, t0, MSTATUS_MDT_BIT
  csrs mstatus, t0
  MNRET
  .align 2
m_rnmi_nopad:
  nop
m_rnmi_back:
  csrsi CSR_MNSTATUS, MNSTATUS_NMIE
  ret

// The RNMI handler: record mndstatus, curdom and mnepc, then take an ecall,
// which is an RNMI exception, with mdstatus preset to a value it must replace.
// MDT is cleared first so that the ecall is not a double trap.
  .globl rnmi_handler
rnmi_handler:
  li t0, 1
  slli t0, t0, MSTATUS_MDT_BIT
  csrc mstatus, t0
  la t0, rnmi_seen
  csrr t1, CSR_MNDSTATUS
  sd t1, 0(t0)
  csrr t1, CSR_CURDOM
  sd t1, 8(t0)
  csrr t1, CSR_MNEPC
  sd t1, 16(t0)
  li t1, (5 << DSTATUS_XPD_SHIFT) | (PEND(2, 3) << DSTATUS_XPPEND_SHIFT)
  csrw CSR_MDSTATUS, t1
  ecall
  la t0, rnmi_seen
  csrr t1, CSR_MNDSTATUS
  sd t1, 48(t0)
  j m_rnmi_back

// The RNMI exception handler: record mdstatus, mndstatus and mcause, then
// resume after the ecall
  .globl rnmi_exc_handler
rnmi_exc_handler:
  la t0, rnmi_seen
  csrr t1, CSR_MDSTATUS
  sd t1, 24(t0)
  csrr t1, CSR_MNDSTATUS
  sd t1, 32(t0)
  csrr t1, mcause
  sd t1, 40(t0)
  csrr t1, mepc
  addi t1, t1, 4
  csrw mepc, t1
  mret

  .option pop

  .bss
  .align 3
  .globl rnmi_seen
rnmi_seen:
  .skip 7 * 8
