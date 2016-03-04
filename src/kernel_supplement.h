/* -*- Mode: C++; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

#ifndef RR_KERNEL_SUPPLEMENT_H_
#define RR_KERNEL_SUPPLEMENT_H_

#include <linux/mman.h>
#include <linux/seccomp.h>
#include <sys/ptrace.h>

/* Definitions that should be part of system headers (and maybe are on some but
 * not all systems).
 * This should not contain anything for which rr needs all the definitions
 * across architectures; those definitions belong in kernel_abi.h.
 */

#define PTRACE_EVENT_NONE 0
#define PTRACE_EVENT_STOP 128

#define PTRACE_SYSEMU 31
#define PTRACE_SYSEMU_SINGLESTEP 32

#ifndef PTRACE_O_TRACESECCOMP
#define PTRACE_O_TRACESECCOMP 0x00000080
#define PTRACE_EVENT_SECCOMP_OBSOLETE 8 // ubuntu 12.04
#define PTRACE_EVENT_SECCOMP 7          // ubuntu 12.10 and future kernels
#endif

#ifndef PTRACE_O_EXITKILL
#define PTRACE_O_EXITKILL (1 << 20)
#endif

#ifndef SECCOMP_SET_MODE_STRICT
#define SECCOMP_SET_MODE_STRICT 0
#endif
#ifndef SECCOMP_SET_MODE_FILTER
#define SECCOMP_SET_MODE_FILTER 1
#endif

#ifndef SYS_SECCOMP
#define SYS_SECCOMP 1
#endif

// These are defined by the include/linux/errno.h in the kernel tree.
// Since userspace doesn't see these errnos in normal operation, that
// header apparently isn't distributed with libc.
#define ERESTARTSYS 512
#define ERESTARTNOINTR 513
#define ERESTARTNOHAND 514
#define ERESTART_RESTARTBLOCK 516

// These definitions haven't made it out to current libc-dev packages
// yet.
#ifndef GRND_NONBLOCK
#define GRND_NONBLOCK 0x0001
#define GRND_RANDOM 0x0002
#endif

/* We need to complement sigsets in order to update the Task blocked
 * set, but POSIX doesn't appear to define a convenient helper.  So we
 * define our own linux-compatible sig_set_t and use bit operators to
 * manipulate sigsets. */
typedef uint64_t sig_set_t;
static_assert(_NSIG / 8 == sizeof(sig_set_t), "Update sig_set_t for _NSIG.");

#ifndef MADV_DONTDUMP
#define MADV_DONTDUMP 16
#endif
#ifndef MADV_DODUMP
#define MADV_DODUMP 17
#endif
#ifndef MADV_SOFT_OFFLINE
#define MADV_SOFT_OFFLINE 101
#endif

#ifndef BUS_MCEERR_AR
#define BUS_MCEERR_AR 4
#endif

#ifndef BUS_MCEERR_AO
#define BUS_MCEERR_AO 5
#endif

#endif /* RR_KERNEL_SUPPLEMENT_H_ */
