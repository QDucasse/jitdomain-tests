// u-pkey: pkey_alloc, pkey_free and pkey_mprotect. All three fail before
// PR_JITDOMAIN_ENABLE. Keys 1-6 are handed
// out lowest first, key 7 is the DRS key and never handed out, flags and
// init_rights must be 0, and only allocated keys (and key 0) tag pages.
// What a tag does to accesses is u-access's business.

#include "user.h"

int main(void)
{
  long page = sysconf(_SC_PAGESIZE);
  char *p;

  user_init();
  p = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK_EQ(p == MAP_FAILED, 0);

  CHECK_EQ(jd_pkey_alloc(0, 0), -EINVAL);
  CHECK_EQ(jd_pkey_free(1), -EINVAL);
  CHECK_EQ(jd_pkey_mprotect(p, page, PROT_READ, 0), -EINVAL);
  CHECK_EQ(jd_enable(), 0);

  // flags and init_rights must be 0
  CHECK_EQ(jd_pkey_alloc(1, 0), -EINVAL);
  CHECK_EQ(jd_pkey_alloc(0, 1), -EINVAL);

  // 1-6, lowest first, then ENOSPC
  for (long k = 1; k < DRSKEY; k++)
    CHECK_EQ(jd_pkey_alloc(0, 0), k);
  CHECK_EQ(jd_pkey_alloc(0, 0), -ENOSPC);

  // A freed key is handed out again, before any higher one
  CHECK_EQ(jd_pkey_free(5), 0);
  CHECK_EQ(jd_pkey_free(3), 0);
  CHECK_EQ(jd_pkey_free(3), -EINVAL);
  CHECK_EQ(jd_pkey_alloc(0, 0), 3);
  CHECK_EQ(jd_pkey_alloc(0, 0), 5);
  CHECK_EQ(jd_pkey_alloc(0, 0), -ENOSPC);

  // Keys 0 and 7, and keys out of range, are never allocated
  CHECK_EQ(jd_pkey_free(0), -EINVAL);
  CHECK_EQ(jd_pkey_free(DRSKEY), -EINVAL);
  CHECK_EQ(jd_pkey_free(8), -EINVAL);
  CHECK_EQ(jd_pkey_free(-1), -EINVAL);

  // pkey_mprotect: allocated keys, key 0, and -1 (keys left alone)
  CHECK_EQ(jd_pkey_mprotect(p, page, PROT_READ | PROT_WRITE, 1), 0);
  CHECK_EQ(jd_pkey_mprotect(p, 2 * page, PROT_READ, 6), 0);
  CHECK_EQ(jd_pkey_mprotect(p, page, PROT_READ | PROT_WRITE, 0), 0);
  CHECK_EQ(jd_pkey_mprotect(p, page, PROT_READ, -1), 0);

  // The DRS key, unallocated keys and keys out of range are rejected
  CHECK_EQ(jd_pkey_mprotect(p, page, PROT_READ, DRSKEY), -EINVAL);
  CHECK_EQ(jd_pkey_free(4), 0);
  CHECK_EQ(jd_pkey_mprotect(p, page, PROT_READ, 4), -EINVAL);
  CHECK_EQ(jd_pkey_mprotect(p, page, PROT_READ, 8), -EINVAL);
  CHECK_EQ(jd_pkey_mprotect(p, page, PROT_READ, -2), -EINVAL);

  // The mprotect part is checked as for mprotect
  CHECK_EQ(jd_pkey_mprotect(p + 1, page, PROT_READ, 1), -EINVAL);
  CHECK_EQ(jd_pkey_mprotect(p, 0, PROT_READ, 1), 0);

  // A freed key leaves its pages tagged; that is the program's business
  CHECK_EQ(jd_pkey_mprotect(p, page, PROT_READ, 2), 0);
  CHECK_EQ(jd_pkey_free(2), 0);
  CHECK_EQ(jd_pkey_alloc(0, 0), 2);

  return user_done("u-pkey");
}
