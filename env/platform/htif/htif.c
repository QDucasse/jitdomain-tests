// HTIF console and exit through tohost

#include "env.h"

volatile uint64_t tohost __attribute__((section(".tohost"), aligned(64)));
volatile uint64_t fromhost __attribute__((section(".tohost"), aligned(64)));

// Device 1 (console), command 1 (putchar); exit is (code << 1) | 1.
// Spike's console does not acknowledge a putchar through fromhost.
void platform_putchar(char c)
{
  while (tohost)
    ;
  tohost = (1ULL << 56) | (1ULL << 48) | (unsigned char)c;
}

void exit(int code)
{
  while (tohost)
    ;
  for (;;)
    tohost = ((uint64_t)code << 1) | 1;
}
