// T-TYPED: domain-typed loads and stores. A typed access executed in
// domain d is checked as e = dtarget[d]; dtarget resets to the identity.
// Also covers T-ORD: an ordinary access to the same page uses e = curdom.

#include "env.h"

STUB(stub_lb); STUB(stub_lh); STUB(stub_lw); STUB(stub_ld);
STUB(stub_lbu); STUB(stub_lhu); STUB(stub_lwu);
STUB(stub_sb); STUB(stub_sh); STUB(stub_sw); STUB(stub_sd);
STUB(stub_dlb); STUB(stub_dlh); STUB(stub_dlw); STUB(stub_dld);
STUB(stub_dlbu); STUB(stub_dlhu); STUB(stub_dlwu);
STUB(stub_dsb); STUB(stub_dsh); STUB(stub_dsw); STUB(stub_dsd);
STUB(stub_dlbu_m2048); STUB(stub_dlbu_p2047);
STUB(stub_dsb_m2048); STUB(stub_dsb_p2047);
STUB(stub_dl_rsvd); STUB(stub_ds_rsvd);

#define DATA 0xf1e2d3c4b5a69788ULL
#define STORED 0x0102030405060708ULL
#define FILL 0xaaaaaaaaaaaaaaaaULL

static const struct {
  char *typed, *ordinary;
  uint64_t want;
} loads[] = {
  { stub_dlb,  stub_lb,  0xffffffffffffff88ULL },
  { stub_dlh,  stub_lh,  0xffffffffffff9788ULL },
  { stub_dlw,  stub_lw,  0xffffffffb5a69788ULL },
  { stub_dld,  stub_ld,  DATA },
  { stub_dlbu, stub_lbu, 0x88 },
  { stub_dlhu, stub_lhu, 0x9788 },
  { stub_dlwu, stub_lwu, 0xb5a69788ULL },
};

static const struct {
  char *typed, *ordinary;
  uint64_t mask;
} stores[] = {
  { stub_dsb, stub_sb, 0xff },
  { stub_dsh, stub_sh, 0xffff },
  { stub_dsw, stub_sw, 0xffffffffULL },
  { stub_dsd, stub_sd, ~0ULL },
};

#define N(a) (sizeof(a) / sizeof(a[0]))

static volatile uint64_t *word(int k, unsigned off)
{
  return (volatile uint64_t *)(PAGE(k) + off);
}

static volatile uint8_t *byte(int k, unsigned off)
{
  return (volatile uint8_t *)(PAGE(k) + off);
}

// DOM1 runs from key 0 and may access K1; DOM2 may access K2; DOM1 may not
static void setup(void)
{
  dom_reset();
  for (int d = 0; d < NDOMS; d++)
    rights_set(d, 0, DR_X);
  rights_set(1, 1, DR_R | DR_W);
  rights_set(2, 2, DR_R | DR_W);
  rights_apply();
  *word(2, 0) = DATA;
  *word(1, 0) = DATA;
}

static void set_target(int d, int t)
{
  uint64_t v = csr_read(CSR_DTARGET);
  v &= ~(7ULL << (3 * d));
  v |= (uint64_t)t << (3 * d);
  csr_write(CSR_DTARGET, v);
}

// With dtarget[1] = 2, typed accesses from DOM1 reach K2; ordinary ones fault
static void check_target(void)
{
  setup();
  set_target(1, 2);

  for (unsigned i = 0; i < N(loads); i++) {
    CHECK_OK(run_u(loads[i].typed, 1, PAGE(2), 0), PRV_U);
    CHECK_EQ(last_trap.a0, loads[i].want);
    CHECK_DOM_FAULT(run_u(loads[i].ordinary, 1, PAGE(2), 0), PAGE(2));
    CHECK_EQ(last_trap.epc, (uintptr_t)loads[i].ordinary);
  }

  for (unsigned i = 0; i < N(stores); i++) {
    *word(2, 16) = FILL;
    CHECK_OK(run_u(stores[i].typed, 1, PAGE(2) + 16, STORED), PRV_U);
    CHECK_EQ(*word(2, 16), (FILL & ~stores[i].mask) | (STORED & stores[i].mask));

    *word(2, 16) = FILL;
    CHECK_DOM_FAULT(run_u(stores[i].ordinary, 1, PAGE(2) + 16, STORED), PAGE(2) + 16);
    CHECK_EQ(*word(2, 16), FILL);
  }

  // The target's rights apply, not the caller's: DOM1 may access K1, DOM2 may not
  CHECK_DOM_FAULT(run_u(stub_dld, 1, PAGE(1), 0), PAGE(1));
  CHECK_OK(run_u(stub_ld, 1, PAGE(1), 0), PRV_U);
  CHECK_EQ(last_trap.a0, DATA);

  // Each right is checked separately: R for typed loads, W for typed stores
  rights_set(2, 2, DR_W);
  rights_apply();
  CHECK_DOM_FAULT(run_u(stub_dlb, 1, PAGE(2), 0), PAGE(2));
  CHECK_OK(run_u(stub_dsb, 1, PAGE(2) + 8, 0x5a), PRV_U);
  CHECK_EQ(*byte(2, 8), 0x5a);
  rights_set(2, 2, DR_R);
  rights_apply();
  CHECK_OK(run_u(stub_dlb, 1, PAGE(2), 0), PRV_U);
  CHECK_DOM_FAULT(run_u(stub_dsb, 1, PAGE(2) + 8, 0xa5), PAGE(2) + 8);
  CHECK_EQ(*byte(2, 8), 0x5a);
}

// The full 12-bit offset: -2048 and +2047 around the middle of the page, and
// offsets that leave the page and land in K1 or K3
static void check_offsets(void)
{
  setup();
  set_target(1, 2);
  *byte(2, 0) = 0x11;
  *byte(2, 4095) = 0x22;

  CHECK_OK(run_u(stub_dlbu_m2048, 1, PAGE(2) + 2048, 0), PRV_U);
  CHECK_EQ(last_trap.a0, 0x11);
  CHECK_OK(run_u(stub_dlbu_p2047, 1, PAGE(2) + 2048, 0), PRV_U);
  CHECK_EQ(last_trap.a0, 0x22);

  CHECK_OK(run_u(stub_dsb_m2048, 1, PAGE(2) + 2048, 0x33), PRV_U);
  CHECK_EQ(*byte(2, 0), 0x33);
  CHECK_OK(run_u(stub_dsb_p2047, 1, PAGE(2) + 2048, 0x44), PRV_U);
  CHECK_EQ(*byte(2, 4095), 0x44);

  CHECK_DOM_FAULT(run_u(stub_dlbu_m2048, 1, PAGE(2), 0), PAGE(1) + 2048);
  CHECK_DOM_FAULT(run_u(stub_dsb_p2047, 1, PAGE(2) + 2049, 0), PAGE(3));
}

// Reset value: dtarget[d] = d, so typed accesses behave like ordinary ones
static void check_identity(void)
{
  setup();
  CHECK_EQ(csr_read(CSR_DTARGET), dtarget_identity());

  for (unsigned i = 0; i < N(loads); i++) {
    CHECK_DOM_FAULT(run_u(loads[i].typed, 1, PAGE(2), 0), PAGE(2));
    CHECK_OK(run_u(loads[i].typed, 1, PAGE(1), 0), PRV_U);
    CHECK_EQ(last_trap.a0, loads[i].want);
    CHECK_OK(run_u(loads[i].typed, 2, PAGE(2), 0), PRV_U);
    CHECK_EQ(last_trap.a0, loads[i].want);
  }
  for (unsigned i = 0; i < N(stores); i++) {
    *word(1, 16) = FILL;
    CHECK_OK(run_u(stores[i].typed, 1, PAGE(1) + 16, STORED), PRV_U);
    CHECK_EQ(*word(1, 16), (FILL & ~stores[i].mask) | (STORED & stores[i].mask));
    CHECK_DOM_FAULT(run_u(stores[i].typed, 1, PAGE(2) + 16, STORED), PAGE(2) + 16);
  }
}

// dtarget is indexed by curdom: only DOM3 is retargeted
static void check_per_domain(void)
{
  setup();
  set_target(3, 2);
  CHECK_OK(run_u(stub_dld, 3, PAGE(2), 0), PRV_U);
  CHECK_EQ(last_trap.a0, DATA);
  CHECK_DOM_FAULT(run_u(stub_dld, 1, PAGE(2), 0), PAGE(2));
  CHECK_DOM_FAULT(run_u(stub_ld, 3, PAGE(2), 0), PAGE(2));

  // Every domain can target every domain
  for (int d = 0; d < NDOMS; d++) {
    for (int t = 0; t < NDOMS; t++) {
      csr_write(CSR_DTARGET, dtarget_identity());
      set_target(d, t);
      uint64_t cause = run_u(stub_dld, d, PAGE(2), 0);
      // Only DOM2 may read K2
      if (t == 2) {
        CHECK_OK(cause, PRV_U);
      } else {
        CHECK_DOM_FAULT(cause, PAGE(2));
      }
    }
  }
}

// Typed accesses never reach DRS pages, whatever the target's rights
static void check_drs_key(void)
{
  setup();
  set_target(1, 2);
  rights_set(2, 7, DR_R | DR_W);
  rights_apply();
  CHECK_DOM_FAULT(run_u(stub_dld, 1, PAGE(7), 0), PAGE(7));
  CHECK_DOM_FAULT(run_u(stub_dsd, 1, PAGE(7), 0), PAGE(7));
}

// S mode: typed accesses are checked only with dstatus.SCHK, through dtarget too
static void check_supervisor(void)
{
  setup();
  set_target(1, 2);
  CHECK_OK(run_s(stub_ld, 1, PAGE(2), 0), PRV_S);
  CHECK_OK(run_s(stub_dld, 1, PAGE(3), 0), PRV_S);

  csr_set(CSR_DSTATUS, DSTATUS_SCHK);
  CHECK_OK(run_s(stub_dld, 1, PAGE(2), 0), PRV_S);
  CHECK_EQ(last_trap.a0, DATA);
  CHECK_DOM_FAULT(run_s(stub_ld, 1, PAGE(2), 0), PAGE(2));
  CHECK_DOM_FAULT(run_s(stub_dld, 1, PAGE(3), 0), PAGE(3));
}

// dstatus.EN = 0: no check at all
static void check_disabled(void)
{
  setup();
  dom_disable();
  CHECK_OK(run_u(stub_dld, 1, PAGE(2), 0), PRV_U);
  CHECK_OK(run_u(stub_dld, 4, PAGE(5), 0), PRV_U);
  CHECK_OK(run_u(stub_dsd, 4, PAGE(5), 0), PRV_U);
}

// funct3 = 111 is reserved in both opcodes
static void check_reserved(void)
{
  setup();
  set_target(1, 2);
  CHECK_EQ(run_u(stub_dl_rsvd, 1, PAGE(2), 0), CAUSE_ILLEGAL_INSN);
  CHECK_EQ(run_u(stub_ds_rsvd, 1, PAGE(2), 0), CAUSE_ILLEGAL_INSN);
  CHECK_EQ(run_s(stub_dl_rsvd, 1, PAGE(2), 0), CAUSE_ILLEGAL_INSN);
}

int main(void)
{
  check_target();
  check_offsets();
  check_identity();
  check_per_domain();
  check_drs_key();
  check_supervisor();
  check_disabled();
  check_reserved();
  return report("t-typed");
}
