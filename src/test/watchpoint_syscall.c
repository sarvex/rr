/* -*- Mode: C; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

#include "rrutil.h"

static void breakpoint(void) {}

static char* p;

int main(void) {
  int fd = open("/dev/zero", O_RDONLY);
  test_assert(fd >= 0);

  p = (char*)mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  test_assert(p != MAP_FAILED);

  breakpoint();

  *p = 'a';

  test_assert(1 == read(fd, p, 1));
  test_assert(*p == 0);

  *p = 'b';

  test_assert(p == mmap(p, PAGE_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0));
  test_assert(*p == 0);

  test_assert(0 == munmap(p, PAGE_SIZE));

  atomic_puts("EXIT-SUCCESS");

  return 0;
}
