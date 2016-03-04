/* -*- Mode: C; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

#include "rrutil.h"

int main(void) {
  int pipe_fds[2];

  test_assert(0 == pipe(pipe_fds));
  test_assert(0 == ioctl(pipe_fds[0], FIOCLEX));
  test_assert(FD_CLOEXEC == fcntl(pipe_fds[0], F_GETFD));
  test_assert(0 == ioctl(pipe_fds[0], FIONCLEX));
  test_assert(0 == fcntl(pipe_fds[0], F_GETFD));

  atomic_puts("EXIT-SUCCESS");
  return 0;
}
