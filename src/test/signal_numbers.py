from rrutil import *

gdb_signals = [
  '',
  'SIGHUP',
  'SIGINT',
  'SIGQUIT',
  'SIGILL',
  'SIGTRAP',
  'SIGABRT',
  'SIGBUS',
  'SIGFPE',
  '#SIGKILL',
  'SIGUSR1',
  'SIGSEGV',
  'SIGUSR2',
  'SIGPIPE',
  'SIGALRM',
  'SIGTERM',
  '#SIGSTKFLT',
  'SIGCHLD',
  'SIGCONT',
  '#SIGSTOP',
  'SIGTSTP',
  'SIGTTIN',
  'SIGTTOU',
  'SIGURG',
  'SIGXCPU',
  'SIGXFSZ',
  'SIGVTALRM',
  'SIGPROF',
  'SIGWINCH',
  'SIGIO',
  '#SIGPWR',
  'SIGSYS']

for sig in xrange(32,65):
    gdb_signals.append('SIG%d'%sig)

for sig in xrange(1,65):
    gdb_sig = gdb_signals[sig]
    if not gdb_sig.startswith('#'):
        send_gdb('handle %s stop'%gdb_sig)
        if gdb_sig == 'SIGINT' or gdb_sig == 'SIGTRAP':
            send_gdb('y')
        send_gdb('c')
        expect_gdb('received signal %s'%gdb_sig)

ok()
