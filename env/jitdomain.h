// JITDomain constants and,
// for assembly sources, .insn macros for the custom instructions so that a
// stock toolchain can assemble them.

#ifndef JITDOMAIN_H
#define JITDOMAIN_H

// CSRs
#define CSR_DSTATUS  0x5c0
#define CSR_DRIGHTS0 0x5c1
#define CSR_DRIGHTS1 0x5c2
#define CSR_DRIGHTS2 0x5c3
#define CSR_DTARGET  0x5c4
#define CSR_DGATE    0x5c5
#define CSR_DSP      0x5c6
#define CSR_DCAUSE   0xdc0
#define CSR_CURDOM   0xcc0
#define CSR_DMPCFG0  0x7c3
#define CSR_DMPCFG1  0x7c4
#define CSR_MDSTATUS 0x7c5
#define CSR_MNDSTATUS 0x7c6

// dstatus / mdstatus; xPPEND packs {valid, label[8:0], dom[2:0]}
#define DSTATUS_EN         0x1
#define DSTATUS_XPD_SHIFT  1
#define DSTATUS_XPD        (0x7 << DSTATUS_XPD_SHIFT)
#define DSTATUS_XPPEND_SHIFT 4
#define DSTATUS_XPPEND     (0x1fff << DSTATUS_XPPEND_SHIFT)
#define DSTATUS_DRSKEY_SHIFT 17
#define DSTATUS_DRSKEY     (0x7 << DSTATUS_DRSKEY_SHIFT)
#define DSTATUS_SCHK       (1 << 20)
#define DSTATUS_MASK       0x1fffff
#define MDSTATUS_MASK      (DSTATUS_XPD | DSTATUS_XPPEND)

#define PEND(dom, label)   (0x1000 | ((label) << 3) | (dom))

// curdom: {dpend_valid, dpend_label[8:0], dpend_dom[2:0], curdom[2:0]}
#define CURDOM_DOM         0x7

// drights entry {X, W, R}
#define DR_R 1
#define DR_W 2
#define DR_X 4

#define DMPCFG_L 0x8

// Causes and dcause subcodes
#define CAUSE_DOMAIN_ACCESS 24
#define CAUSE_DOMAIN_CFI    25
#define DCAUSE_LPAD 1
#define DCAUSE_DRS  2
#define DCAUSE_GATE 3

#define NDOMS 8
#define NKEYS 8

#ifdef __ASSEMBLER__

// Typed loads (custom-0) and stores (custom-1): dl* rd, off(rs1) / ds* rs2, off(rs1)
.macro dlb rd, addr
  .insn i 0x0b, 0, \rd, \addr
.endm
.macro dlh rd, addr
  .insn i 0x0b, 1, \rd, \addr
.endm
.macro dlw rd, addr
  .insn i 0x0b, 2, \rd, \addr
.endm
.macro dld rd, addr
  .insn i 0x0b, 3, \rd, \addr
.endm
.macro dlbu rd, addr
  .insn i 0x0b, 4, \rd, \addr
.endm
.macro dlhu rd, addr
  .insn i 0x0b, 5, \rd, \addr
.endm
.macro dlwu rd, addr
  .insn i 0x0b, 6, \rd, \addr
.endm
.macro dsb rs2, addr
  .insn s 0x2b, 0, \rs2, \addr
.endm
.macro dsh rs2, addr
  .insn s 0x2b, 1, \rs2, \addr
.endm
.macro dsw rs2, addr
  .insn s 0x2b, 2, \rs2, \addr
.endm
.macro dsd rs2, addr
  .insn s 0x2b, 3, \rs2, \addr
.endm

// Transitions (custom-2): dom in imm[11:9], label in imm[8:0]. The
// immediate is sign-adjusted because .insn takes a signed 12-bit value.
.macro domcall rd, rs1, dom, label
  .insn i 0x5b, 0, \rd, \rs1, (((((\dom) << 9) | (\label)) ^ 0x800) - 0x800)
.endm
.macro domret
  .insn i 0x5b, 1, x0, x0, 0
.endm
.macro domlpad dom, label
  .insn i 0x5b, 2, x0, x0, (((((\dom) << 9) | (\label)) ^ 0x800) - 0x800)
.endm

#endif

#endif
