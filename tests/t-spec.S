// t-spec stubs

#include "jitdomain.h"

#define STUB(name, ...) \
  .globl name, name##_end; .align 2; name: __VA_ARGS__; ecall; name##_end:

  .text

// Load the secret at a0, then load the line of the probe array at a1 that
// its low three bits select (64 bytes per value). When the first load is
// denied, the probe load depends on data that must never be returned.
STUB(spec_probe, ld t0, 0(a0); andi t0, t0, 7; slli t0, t0, 6; add t0, t0, a1; ld t1, 0(t0))
