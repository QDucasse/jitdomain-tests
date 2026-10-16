#include <stdarg.h>
#include "env.h"

volatile trap_t last_trap;
volatile uint64_t m_traps;
int checks, failures;

static void put_u(uint64_t v, unsigned base)
{
  char buf[24];
  int n = 0;
  do {
    buf[n++] = "0123456789abcdef"[v % base];
    v /= base;
  } while (v);
  while (n)
    platform_putchar(buf[--n]);
}

// %s %c %d %u %x, an optional 'l' is accepted and ignored (everything is 64-bit)
int printf(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  for (; *fmt; fmt++) {
    if (*fmt != '%') {
      platform_putchar(*fmt);
      continue;
    }
    if (*++fmt == 'l')
      fmt++;
    switch (*fmt) {
    case 's':
      for (const char *s = va_arg(ap, const char *); *s; s++)
        platform_putchar(*s);
      break;
    case 'c':
      platform_putchar(va_arg(ap, int));
      break;
    case 'd': {
      int64_t v = va_arg(ap, int64_t);
      if (v < 0) {
        platform_putchar('-');
        v = -v;
      }
      put_u(v, 10);
      break;
    }
    case 'u':
      put_u(va_arg(ap, uint64_t), 10);
      break;
    case 'x':
      put_u(va_arg(ap, uint64_t), 16);
      break;
    default:
      platform_putchar(*fmt);
    }
  }
  va_end(ap);
  return 0;
}

void *memcpy(void *dst, const void *src, size_t n)
{
  char *d = dst;
  const char *s = src;
  while (n--)
    *d++ = *s++;
  return dst;
}

void *memset(void *dst, int c, size_t n)
{
  char *d = dst;
  while (n--)
    *d++ = c;
  return dst;
}

void check_failed(const char *file, int line, const char *expr, uint64_t got, uint64_t want)
{
  failures++;
  if (failures <= 50)
    printf("  FAIL %s:%d: %s = 0x%lx, expected 0x%lx\n", file, line, expr, got, want);
}

int report(const char *name)
{
  printf("%s: %d checks, %d failures\n", name, checks, failures);
  return failures ? 1 : 0;
}

uint64_t handle_trap(uint64_t *regs)
{
  last_trap.cause = csr_read(mcause);
  last_trap.epc = csr_read(mepc);
  last_trap.tval = csr_read(mtval);
  last_trap.dcause = csr_read(CSR_DCAUSE);
  last_trap.mdstatus = csr_read(CSR_MDSTATUS);
  last_trap.a0 = regs[10];

  if (((csr_read(mstatus) >> 11) & 3) != PRV_M)
    return 1;

  // A trap in the monitor itself, expected by the test: skip the instruction
  m_traps++;
  uint16_t insn = *(volatile uint16_t *)last_trap.epc;
  csr_write(mepc, last_trap.epc + ((insn & 3) == 3 ? 4 : 2));
  return 0;
}

// --- Domain configuration ----------------------------------------------------

static uint64_t drights[3];

void rights_clear(void)
{
  drights[0] = drights[1] = drights[2] = 0;
}

// drights2:drights1:drights0, {X, W, R} at bit 3*(8*e+k); an entry may straddle two CSRs
void rights_set(int e, int k, int r)
{
  for (int b = 0; b < 3; b++) {
    unsigned bit = 3 * (NKEYS * e + k) + b;
    drights[bit / 64] &= ~(1ULL << (bit % 64));
    drights[bit / 64] |= (uint64_t)((r >> b) & 1) << (bit % 64);
  }
}

int rights_get(int e, int k)
{
  int r = 0;
  for (int b = 0; b < 3; b++) {
    unsigned bit = 3 * (NKEYS * e + k) + b;
    r |= ((drights[bit / 64] >> (bit % 64)) & 1) << b;
  }
  return r;
}

void rights_apply(void)
{
  csr_write(CSR_DRIGHTS0, drights[0]);
  csr_write(CSR_DRIGHTS1, drights[1]);
  csr_write(CSR_DRIGHTS2, drights[2]);
}

uint64_t dtarget_identity(void)
{
  uint64_t v = 0;
  for (uint64_t d = 0; d < NDOMS; d++)
    v |= d << (3 * d);
  return v;
}

// Keys from the key source (keysrc-*.c); every other CSR at its reset value,
// apart from dstatus = EN | DRSKEY=7
void dom_reset(void)
{
  keysrc_reset();
  rights_clear();
  rights_apply();
  csr_write(CSR_DTARGET, dtarget_identity());
  csr_write(CSR_DGATE, 0);
  csr_write(CSR_DSP, 0);
  csr_write(CSR_MDSTATUS, 0);
  csr_write(CSR_DSTATUS, DSTATUS_EN | (7 << DSTATUS_DRSKEY_SHIFT));
}

void dom_disable(void)
{
  keysrc_disable();
  csr_clear(CSR_DSTATUS, DSTATUS_EN);
}

void *place_stub(uintptr_t dst, const char *start, const char *end)
{
  memcpy((void *)dst, start, end - start);
  fence_i();
  return (void *)dst;
}
