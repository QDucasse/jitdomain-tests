// Switch-cost benchmark stubs. They run in U mode through run_lower().
//
// Loop stubs repeat their body a0 times between two reads of cycle and
// instret: s0/s1 hold the cycle counts before and after the loop, s2/s3 the
// instret counts. Single-shot stubs read cycle into s0, s1, s2 around two
// transitions. Snippets are copied into page 1 for DOM1, so they are position
// independent. No compressed instructions, so that every pad is 4-byte aligned.

#include "jitdomain.h"

#define CSR_CYCLE   0xc00
#define CSR_INSTRET 0xc02

#define STUB(name, ...) \
  .globl name, name##_end; .align 2; name: __VA_ARGS__; ecall; name##_end:
#define SNIP(name, ...) \
  .globl name, name##_end; .align 2; name: __VA_ARGS__; name##_end:

// A loop stub: setup, then the body a0 times
#define LOOP(name, setup, ...) \
  STUB(name, setup; \
       csrr s2, CSR_INSTRET; csrr s0, CSR_CYCLE; \
       1: __VA_ARGS__; addi a0, a0, -1; bnez a0, 1b; \
       csrr s1, CSR_CYCLE; csrr s3, CSR_INSTRET)

#define REPEAT8(...) \
  __VA_ARGS__; __VA_ARGS__; __VA_ARGS__; __VA_ARGS__; \
  __VA_ARGS__; __VA_ARGS__; __VA_ARGS__; __VA_ARGS__

  .text
  .option push
  .option norvc
  .option norelax

// The loop alone, subtracted from every other loop
LOOP(loop_empty, nop, )

// An ordinary call to an empty function
LOOP(loop_call, nop, jal ra, empty_function)
  .align 2
empty_function:
  ret

// A round trip to the domain whose pad is at a1: DOM1 (callee_dom1, copied
// into page 1) or DOM0 (callee_dom0, in the image)
LOOP(loop_round_trip_dom1, nop, domcall ra, a1, 1, 1)
LOOP(loop_round_trip_dom0, nop, domcall ra, a1, 0, 1)
SNIP(callee_dom1, domlpad 1, 1; domret)
SNIP(callee_dom0, domlpad 0, 1; domret)

// Two one-way domcalls per iteration: DOM0 enters DOM1 at a1, which enters
// DOM0 again at the pad after the first domcall (a2). Nothing returns, so the
// DRS grows by two records per iteration.
  .globl loop_one_way, loop_one_way_end
  .align 2
loop_one_way:
  la a2, 2f
  csrr s2, CSR_INSTRET
  csrr s0, CSR_CYCLE
1:
  domcall ra, a1, 1, 1
2:
  domlpad 0, 2
  addi a0, a0, -1
  bnez a0, 1b
  csrr s1, CSR_CYCLE
  csrr s3, CSR_INSTRET
  ecall
loop_one_way_end:
SNIP(enter_dom0, domlpad 1, 1; domcall ra, a2, 0, 2)

// Ordinary and typed accesses to a1, eight per iteration
LOOP(loop_ld,  nop, REPEAT8(ld t0, 0(a1)))
LOOP(loop_dld, nop, REPEAT8(dld t0, 0(a1)))
LOOP(loop_sd,  nop, REPEAT8(sd zero, 0(a1)))
LOOP(loop_dsd, nop, REPEAT8(dsd zero, 0(a1)))

// A trap round trip: ecall to trap_return, which mret's past it. The stub
// ends with an ebreak instead, which trap_return passes on to the harness.
  .globl loop_trap, loop_trap_end
  .align 2
loop_trap:
  csrr s2, CSR_INSTRET
  csrr s0, CSR_CYCLE
1:
  ecall
  addi a0, a0, -1
  bnez a0, 1b
  csrr s1, CSR_CYCLE
  csrr s3, CSR_INSTRET
  ebreak
loop_trap_end:

// The cycle reads alone, subtracted from the single-shot stubs
STUB(single_empty, csrr s0, CSR_CYCLE; csrr s1, CSR_CYCLE; csrr s2, CSR_CYCLE)

// Two round trips to the pad at a1, timed one by one: after a cache
// eviction, the first one is cold and the second one warm
STUB(single_round_trips,
     csrr s0, CSR_CYCLE; domcall ra, a1, 1, 1;
     csrr s1, CSR_CYCLE; domcall ra, a1, 1, 1;
     csrr s2, CSR_CYCLE)

  .option pop

// M-mode trap vector for loop_trap: an ecall from U mode returns past
// itself at once; any other trap goes on to the harness's vector, which the
// benchmark saved in harness_trap_vector. Clobbers t6, which the stub does
// not use.
  .globl trap_return
  .align 2
trap_return:
  csrr t6, mcause
  addi t6, t6, -8
  bnez t6, 1f
  csrr t6, mepc
  addi t6, t6, 4
  csrw mepc, t6
  mret
1:
  la t6, harness_trap_vector
  ld t6, 0(t6)
  jr t6
