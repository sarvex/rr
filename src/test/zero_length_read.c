/* -*- Mode: C; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

#include "rrutil.h"

int main(void) {
  char buf[1024];
  ssize_t count = read(STDIN_FILENO, &buf[0], 0);
  test_assert(count == 0);
  atomic_printf("EXIT-SUCCESS");
  return 0;
}
