// HTIF console and exit through tohost

#include "env.h"

volatile uint64_t tohost __attribute__((section(".tohost"), aligned(64)));
volatile uint64_t fromhost __attribute__((section(".tohost"), aligned(64)));

// marchid of CVA6. Its Verilator testbench reads tohost from the commit trace
// and never clears it, so waiting for the host would hang there.
#define MARCHID_CVA6 3

static bool host_clears_tohost(void)
{
  return csr_read(marchid) != MARCHID_CVA6;
}

// Device 1 (console), command 1 (putchar); exit is (code << 1) | 1.
// Spike's console does not acknowledge a putchar through fromhost.
void platform_putchar(char c)
{
  while (tohost && host_clears_tohost())
    ;
  tohost = (1ULL << 56) | (1ULL << 48) | (unsigned char)c;
}

void exit(int code)
{
  while (tohost && host_clears_tohost())
    ;
  for (;;)
    tohost = ((uint64_t)code << 1) | 1;
}
