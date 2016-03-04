/* -*- Mode: C; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

#include "rrutil.h"

static volatile int vv = 0;

static void breakpoint(void) {}

static void funcall(void) {
  char buf[2000000];
  size_t i;
  for (i = 0; i < sizeof(buf); ++i) {
    buf[i] = (char)i;
  }
  for (i = 0; i < sizeof(buf); ++i) {
    vv += buf[i % 777777];
  }
}

int main(void) {
  char v;
  char* fix_addr;
  void* p;

  breakpoint();

  fix_addr =
      (char*)(((uintptr_t)&v - 256 * 1024) & ~(uintptr_t)(PAGE_SIZE - 1));
  p = mmap(fix_addr, PAGE_SIZE, PROT_READ | PROT_WRITE,
           MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  test_assert(p == fix_addr);

  funcall();

  return 0;
}
