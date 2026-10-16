// T-DEPUTY JIT code and runtime primitives. The JIT snippets are copied into
// the code-zone page and take their arguments from a frame in the heap at a0:
// {a0, a1, a2, target}. The primitives run from the image in DOM0 and are
// entered through labelled pads: 1 = at:, 2 = at:put:, 3 = at: through the
// DOM2 table. Each ByteArray primitive has an ordinary build and a typed one,
// which follows heap-resident pointers with typed accesses (dtarget[0] = 1).

#include "jitdomain.h"

// A snippet copied by the test; ends however its body ends
#define SNIP(name, ...) \
  .globl name, name##_end; .align 2; name: __VA_ARGS__; name##_end:

// A runtime function in the image
#define FUNC(name, ...) \
  .globl name; .align 2; name: __VA_ARGS__

#define TABLE_ENTRIES 2
#define TABLE_TAG_BYTES 0x42

  .text
  .option push
  .option norvc
  .option norelax

// JIT code: arr[i] = v with no bounds check, from {arr, i, v}
SNIP(jit_store,
  ld t0, 0(a0); ld t1, 8(a0); ld t2, 16(a0);
  slli t1, t1, 3; add t0, t0, t1;
  sd t2, 0(t0); ecall)

// JIT code: a typed store of v to addr, from {addr, -, v}
SNIP(jit_typed_store,
  ld t0, 0(a0); ld t2, 16(a0);
  dsd t2, 0(t0); ecall)

// JIT code: call a primitive with {a0, a1, a2} through the trampoline label
.macro jit_call label
  ld t3, 24(a0); ld a2, 16(a0); ld a1, 8(a0); ld a0, 0(a0);
  domcall ra, t3, 0, \label; ecall
.endm
SNIP(jit_call_1, jit_call 1)
SNIP(jit_call_2, jit_call 2)
SNIP(jit_call_3, jit_call 3)

// ByteArray {data, len}: at:(ba, i) returns data[i], at:put:(ba, i, v) stores
// v there; both return -1 when i is out of bounds
FUNC(prim_at,
  domlpad 0, 1;
  ld t0, 8(a0); bgeu a1, t0, 1f;
  ld t0, 0(a0); add t0, t0, a1;
  lbu a0, 0(t0); domret;
1:
  li a0, -1; domret)

FUNC(prim_at_typed,
  domlpad 0, 1;
  dld t0, 8(a0); bgeu a1, t0, 1f;
  dld t0, 0(a0); add t0, t0, a1;
  dlbu a0, 0(t0); domret;
1:
  li a0, -1; domret)

FUNC(prim_at_put,
  domlpad 0, 2;
  ld t0, 8(a0); bgeu a1, t0, 1f;
  ld t0, 0(a0); add t0, t0, a1;
  sb a2, 0(t0); li a0, 0; domret;
1:
  li a0, -1; domret)

FUNC(prim_at_put_typed,
  domlpad 0, 2;
  dld t0, 8(a0); bgeu a1, t0, 1f;
  dld t0, 0(a0); add t0, t0, a1;
  dsb a2, 0(t0); li a0, 0; domret;
1:
  li a0, -1; domret)

// A handle {idx} names an entry {ptr, len, tag} of the DOM2 table, which the
// runtime trusts: at:(handle, i) checks the index and the tag, then returns
// ptr[i]
FUNC(prim_at_handle,
  domlpad 0, 3;
  dld t0, 0(a0);
  li t1, TABLE_ENTRIES; bgeu t0, t1, 1f;
  li t1, 24; mul t0, t0, t1;
  la t1, deputy_table; ld t1, 0(t1); add t0, t0, t1;
  ld t1, 16(t0); li t2, TABLE_TAG_BYTES; bne t1, t2, 1f;
  ld t1, 8(t0); bgeu a1, t1, 1f;
  ld t0, 0(t0); add t0, t0, a1;
  lbu a0, 0(t0); domret;
1:
  li a0, -1; domret)

  .option pop
