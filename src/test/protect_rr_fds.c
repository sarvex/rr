/* -*- Mode: C; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

#include "rrutil.h"

int main(int argc, char* argv[]) {
  pid_t child;
  int status;
  int ret;
  int fd;
  int pipe_fds[2];
  struct rlimit nofile;

  if (argc == 2) {
    atomic_puts("EXIT-SUCCESS");
    return 77;
  }

  /* Various spawning APIs try to close all open file descriptors before
     exec --- via direct close(), or by setting CLOEXEC. Check that
     those don't interfere with rr by closing RR_RESERVED_ROOT_DIR_FD
     or some other essential file descriptor. */
  test_assert(0 == getrlimit(RLIMIT_NOFILE, &nofile));
  for (fd = STDERR_FILENO + 1; fd < (int)nofile.rlim_cur; ++fd) {
    ret = fcntl(fd, F_SETFD, FD_CLOEXEC);
    test_assert(ret == 0 || (ret == -1 && errno == EBADF));
    ret = dup2(STDERR_FILENO, fd);
    test_assert(ret == fd || (ret == -1 && errno == EBADF));
    ret = dup3(STDERR_FILENO, fd, O_CLOEXEC);
    test_assert(ret == fd || (ret == -1 && errno == EBADF));
    ret = close(fd);
    test_assert(ret == 0 || (ret == -1 && errno == EBADF));
  }

  /* Check that syscall buffering still works */
  test_assert(0 == pipe(pipe_fds));
  test_assert(1 == write(pipe_fds[1], "c", 1));

  if (0 == (child = fork())) {
    execl(argv[0], argv[0], "step2", NULL);
  }
  test_assert(child == waitpid(child, &status, 0));
  test_assert(WIFEXITED(status) && WEXITSTATUS(status) == 77);
  return 0;
}
