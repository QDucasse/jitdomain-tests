// t-spec: a load denied by the domain check returns no data, not even
// transiently. A stub indexes a probe array with the secret, and the monitor
// then times each probe line. Without cache timing (Spike, QEMU) calibration
// sees no hit/miss difference, and the test skips itself.

#include "env.h"

STUB(spec_probe);

#define SECRET_PAGE 2
#define PROBE_PAGE 3
#define LINE 64
#define NVAL 8

// Read well over the data cache's size from memory past the image, so that
// the probe lines are evicted whatever the replacement policy
#define EVICT_BYTES (256 * 1024)
#define EVICT_STRIDE 16
extern char _end[];

static uintptr_t evict_base(void)
{
  return ((uintptr_t)_end + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1);
}

static void evict(void)
{
  uint64_t sum = 0;
  for (uintptr_t p = evict_base(); p < evict_base() + EVICT_BYTES; p += EVICT_STRIDE)
    sum += *(volatile uint64_t *)p;
  __asm__ volatile("" :: "r"(sum));
}

// Cycles taken by one load; the CSR reads commit in order around it
static uint64_t time_load(uintptr_t a)
{
  uint64_t t0, t1, v;
  __asm__ volatile("csrr %0, mcycle\n\t"
                   "ld %1, 0(%3)\n\t"
                   "add %1, %1, zero\n\t"
                   "csrr %2, mcycle"
                   : "=&r"(t0), "=&r"(v), "=&r"(t1) : "r"(a) : "memory");
  return t1 - t0;
}

static uintptr_t probe_line(int v)
{
  return PAGE(PROBE_PAGE) + (uintptr_t)v * LINE;
}

// Run the stub with the secret readable or not; return the probe lines found
// in the cache afterwards as a bit mask
static unsigned run_probe(bool readable, uint64_t secret, uint64_t threshold, uint64_t *cause)
{
  dom_reset();
  rights_set(1, 0, DR_X);
  rights_set(1, PROBE_PAGE, DR_R);
  if (readable)
    rights_set(1, SECRET_PAGE, DR_R);
  rights_apply();
  *(volatile uint64_t *)PAGE(SECRET_PAGE) = secret;

  evict();
  *cause = run_u(spec_probe, 1, PAGE(SECRET_PAGE), PAGE(PROBE_PAGE));

  unsigned cached = 0;
  for (int v = 0; v < NVAL; v++)
    if (time_load(probe_line(v)) < threshold)
      cached |= 1u << v;
  return cached;
}

int main(void)
{
  // Calibrate on the probe lines: misses right after eviction, then hits.
  // Every miss must be clearly slower than every hit.
  uint64_t miss = ~0ULL, hit = 0;
  for (int v = 0; v < NVAL; v++) {
    evict();
    uint64_t m = time_load(probe_line(v));
    uint64_t h = time_load(probe_line(v));
    miss = m < miss ? m : miss;
    hit = h > hit ? h : hit;
  }
  printf("t-spec: a load takes at least %d cycles on a miss, at most %d on a hit\n", (int)miss, (int)hit);
  if (miss <= hit + 2) {
    printf("t-spec: no cache timing, skipped\n");
    return report("t-spec");
  }
  uint64_t threshold = (hit + miss) / 2;

  static const uint64_t secrets[] = { 3, 5 };
  for (unsigned i = 0; i < sizeof secrets / sizeof secrets[0]; i++) {
    uint64_t s = secrets[i], cause;

    // Control: with the secret readable, the probe line it selects is cached
    unsigned cached = run_probe(true, s, threshold, &cause);
    CHECK_OK(cause, PRV_U);
    CHECK_EQ(cached, 1u << s);

    // Denied: the load faults, and no probe line was loaded
    cached = run_probe(false, s, threshold, &cause);
    CHECK_DOM_FAULT(cause, PAGE(SECRET_PAGE));
    CHECK_EQ(cached, 0);
  }
  return report("t-spec");
}
