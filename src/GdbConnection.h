/* -*- Mode: C++; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

#ifndef RR_GDB_CONNECTION_H_
#define RR_GDB_CONNECTION_H_

#include <stddef.h>
#include <sys/types.h>

#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "GdbRegister.h"
#include "Registers.h"
#include "ReplaySession.h"
#include "ReplayTimeline.h"

/**
 * Descriptor for task within a task group.  Note: on linux, we can
 * uniquely identify any thread by its |tid| (ignoring pid
 * namespaces).
 */
struct GdbThreadId {
  GdbThreadId(pid_t pid = -1, pid_t tid = -1) : pid(pid), tid(tid) {}

  pid_t pid;
  pid_t tid;

  bool operator==(const GdbThreadId& o) const {
    return pid == o.pid && tid == o.tid;
  }

  static const GdbThreadId ANY;
  static const GdbThreadId ALL;
};

inline std::ostream& operator<<(std::ostream& o, const GdbThreadId& t) {
  o << t.pid << "." << t.tid;
  return o;
}

/**
 * Represents a possibly-undefined register |name|.  |size| indicates how
 * many bytes of |value| are valid, if any.
 */
struct GdbRegisterValue {
  enum { MAX_SIZE = Registers::MAX_SIZE };
  GdbRegister name;
  union {
    uint8_t value[MAX_SIZE];
    uint8_t value1;
    uint16_t value2;
    uint32_t value4;
    uint64_t value8;
  };
  size_t size;
  bool defined;
};

/**
 * Represents the register file, indexed by |DbgRegister| values
 * above.
 */
struct GdbRegisterFile {
  std::vector<GdbRegisterValue> regs;

  GdbRegisterFile(size_t n_regs) : regs(n_regs){};

  size_t total_registers() const { return regs.size(); }
};

enum GdbRequestType {
  DREQ_NONE = 0,

  /* None of these requests have parameters. */
  DREQ_GET_CURRENT_THREAD,
  DREQ_GET_OFFSETS,
  DREQ_GET_REGS,
  DREQ_GET_STOP_REASON,
  DREQ_GET_THREAD_LIST,
  DREQ_INTERRUPT,
  DREQ_DETACH,

  /* These use params.target. */
  DREQ_GET_AUXV,
  DREQ_GET_IS_THREAD_ALIVE,
  DREQ_GET_THREAD_EXTRA_INFO,
  DREQ_SET_CONTINUE_THREAD,
  DREQ_SET_QUERY_THREAD,
  // gdb wants to write back siginfo_t to a tracee.  More
  // importantly, this packet arrives before an experiment
  // session for a |call foo()| is about to be torn down.
  //
  // TODO: actual interface NYI.
  DREQ_WRITE_SIGINFO,

  /* These use params.mem. */
  DREQ_GET_MEM,
  DREQ_SET_MEM,
  // gdb wants to read the current siginfo_t for a stopped
  // tracee.  More importantly, this packet arrives at the very
  // beginning of a |call foo()| experiment.
  //
  // Uses .mem for offset/len.
  DREQ_READ_SIGINFO,
  DREQ_SEARCH_MEM,
  DREQ_MEM_FIRST = DREQ_GET_MEM,
  DREQ_MEM_LAST = DREQ_SEARCH_MEM,

  DREQ_REMOVE_SW_BREAK,
  DREQ_REMOVE_HW_BREAK,
  DREQ_REMOVE_WR_WATCH,
  DREQ_REMOVE_RD_WATCH,
  DREQ_REMOVE_RDWR_WATCH,
  DREQ_SET_SW_BREAK,
  DREQ_SET_HW_BREAK,
  DREQ_SET_WR_WATCH,
  DREQ_SET_RD_WATCH,
  DREQ_SET_RDWR_WATCH,
  DREQ_WATCH_FIRST = DREQ_REMOVE_SW_BREAK,
  DREQ_WATCH_LAST = DREQ_SET_RDWR_WATCH,

  /* Use params.reg. */
  DREQ_GET_REG,
  DREQ_SET_REG,
  DREQ_REG_FIRST = DREQ_GET_REG,
  DREQ_REG_LAST = DREQ_SET_REG,

  /* Use params.cont. */
  DREQ_CONT,

  /* gdb host detaching from stub.  No parameters. */

  /* Uses params.restart. */
  DREQ_RESTART,

  /* Uses params.text. */
  DREQ_RR_CMD,
};

enum GdbRestartType {
  RESTART_FROM_PREVIOUS,
  RESTART_FROM_EVENT,
  RESTART_FROM_CHECKPOINT,
};

enum GdbActionType { ACTION_CONTINUE, ACTION_STEP };

struct GdbContAction {
  GdbContAction(GdbActionType type = ACTION_CONTINUE,
                const GdbThreadId& target = GdbThreadId::ANY,
                int signal_to_deliver = 0)
      : type(type), target(target), signal_to_deliver(signal_to_deliver) {}
  GdbActionType type;
  GdbThreadId target;
  int signal_to_deliver;
};

/**
 * These requests are made by the debugger host and honored in proxy
 * by rr, the target.
 */
struct GdbRequest {
  GdbRequest(GdbRequestType type = DREQ_NONE)
      : type(type), suppress_debugger_stop(false) {}
  GdbRequest(const GdbRequest& other)
      : type(other.type),
        target(other.target),
        suppress_debugger_stop(other.suppress_debugger_stop),
        mem_(other.mem_),
        watch_(other.watch_),
        reg_(other.reg_),
        restart_(other.restart_),
        cont_(other.cont_),
        text_(other.text_) {}
  GdbRequest& operator=(const GdbRequest& other) {
    this->~GdbRequest();
    new (this) GdbRequest(other);
    return *this;
  }

  const GdbRequestType type;
  GdbThreadId target;
  bool suppress_debugger_stop;

  struct Mem {
    uintptr_t addr;
    size_t len;
    // For SET_MEM requests, the |len| raw bytes that are to be written.
    // For SEARCH_MEM requests, the bytes to search for.
    std::vector<uint8_t> data;
  } mem_;
  struct Watch {
    uintptr_t addr;
    int kind;
    std::vector<std::vector<uint8_t> > conditions;
  } watch_;
  GdbRegisterValue reg_;
  struct Restart {
    int param;
    std::string param_str;
    GdbRestartType type;
  } restart_;
  struct Cont {
    RunDirection run_direction;
    std::vector<GdbContAction> actions;
  } cont_;
  std::string text_;

  Mem& mem() {
    assert(type >= DREQ_MEM_FIRST && type <= DREQ_MEM_LAST);
    return mem_;
  }
  const Mem& mem() const {
    assert(type >= DREQ_MEM_FIRST && type <= DREQ_MEM_LAST);
    return mem_;
  }
  Watch& watch() {
    assert(type >= DREQ_WATCH_FIRST && type <= DREQ_WATCH_LAST);
    return watch_;
  }
  const Watch& watch() const {
    assert(type >= DREQ_WATCH_FIRST && type <= DREQ_WATCH_LAST);
    return watch_;
  }
  GdbRegisterValue& reg() {
    assert(type >= DREQ_REG_FIRST && type <= DREQ_REG_LAST);
    return reg_;
  }
  const GdbRegisterValue& reg() const {
    assert(type >= DREQ_REG_FIRST && type <= DREQ_REG_LAST);
    return reg_;
  }
  Restart& restart() {
    assert(type == DREQ_RESTART);
    return restart_;
  }
  const Restart& restart() const {
    assert(type == DREQ_RESTART);
    return restart_;
  }
  Cont& cont() {
    assert(type == DREQ_CONT);
    return cont_;
  }
  const Cont& cont() const {
    assert(type == DREQ_CONT);
    return cont_;
  }
  const std::string& text() const {
    assert(type == DREQ_RR_CMD);
    return text_;
  }

  /**
   * Return nonzero if this requires that program execution be resumed
   * in some way.
   */
  bool is_resume_request() const { return type == DREQ_CONT; }
};

/**
 * This struct wraps up the state of the gdb protocol, so that we can
 * offer a (mostly) stateless interface to clients.
 */
class GdbConnection {
public:
  /**
   * Wait for exactly one gdb host to connect to this remote target on
   * IP address 127.0.0.1, port |port|.  If |probe| is nonzero, a unique
   * port based on |start_port| will be searched for.  Otherwise, if
   * |port| is already bound, this function will fail.
   *
   * Pass the |tgid| of the task on which this debug-connection request
   * is being made.  The remaining debugging session will be limited to
   * traffic regarding |tgid|, but clients don't need to and shouldn't
   * need to assume that.
   *
   * If we're opening this connection on behalf of a known client, pass
   * an fd in |client_params_fd|; we'll write the allocated port and |exe_image|
   * through the fd before waiting for a connection. |exe_image| is the
   * process that will be debugged by client, or null ptr if there isn't
   * a client.
   *
   * This function is infallible: either it will return a valid
   * debugging context, or it won't return.
   */
  enum ProbePort { DONT_PROBE = 0, PROBE_PORT };
  struct Features {
    Features() : reverse_execution(true) {}
    bool reverse_execution;
  };
  static std::unique_ptr<GdbConnection> await_client_connection(
      unsigned short desired_port, ProbePort probe, pid_t tgid,
      const std::string& exe_image, const Features& features,
      ScopedFd* client_params_fd = nullptr);

  /**
   * Exec gdb using the params that were written to
   * |params_pipe_fd|.  Optionally, pre-define in the gdb client the set
   * of macros defined in |macros| if nonnull.
   */
  static void launch_gdb(ScopedFd& params_pipe_fd, const std::string& macros,
                         const std::string& gdb_command_file_path,
                         const std::string& gdb_binary_file_path);

  /**
   * Call this when the target of |req| is needed to fulfill the
   * request, but the target is dead.  This situation is a symptom of a
   * gdb or rr bug.
   */
  void notify_no_such_thread(const GdbRequest& req);

  /**
   * Finish a DREQ_RESTART request.  Should be invoked after replay
   * restarts and prior GdbConnection has been restored.
   */
  void notify_restart();

  /**
   * Return the current request made by the debugger host, that needs to
   * be satisfied.  This function will block until either there's a
   * debugger host request that needs a response, or until a request is
   * made to resume execution of the target.  In the latter case,
   * calling this function multiple times will return an appropriate
   * resume request each time (see above).
   *
   * The target should peek at the debugger request in between execution
   * steps.  A new request may need to be serviced.
   */
  GdbRequest get_request();

  /**
   * Notify the host that this process has exited with |code|.
   */
  void notify_exit_code(int code);

  /**
   * Notify the host that this process has exited from |sig|.
   */
  void notify_exit_signal(int sig);

  /**
   * Notify the host that a resume request has "finished", i.e., the
   * target has stopped executing for some reason.  |sig| is the signal
   * that stopped execution, or 0 if execution stopped otherwise.
   */
  void notify_stop(GdbThreadId which, int sig, uintptr_t watch_addr = 0);

  /** Notify the debugger that a restart request failed. */
  void notify_restart_failed();

  /**
   * Tell the host that |thread| is the current thread.
   */
  void reply_get_current_thread(GdbThreadId thread);

  /**
   * Reply with the target thread's |auxv| pairs. |auxv.empty()|
   * if there was an error reading the auxiliary vector.
   */
  void reply_get_auxv(const std::vector<uint8_t>& auxv);

  /**
   * |alive| is true if the requested thread is alive, false if dead.
   */
  void reply_get_is_thread_alive(bool alive);

  /**
   * |info| is a string containing data about the request target that
   * might be relevant to the debugger user.
   */
  void reply_get_thread_extra_info(const std::string& info);

  /**
   * |ok| is true if req->target can be selected, false otherwise.
   */
  void reply_select_thread(bool ok);

  /**
   * The first |mem.size()| bytes of the request were read into |mem|.
   * |mem.size()| must be less than or equal to the length of the request.
   */
  void reply_get_mem(const std::vector<uint8_t>& mem);

  /**
   * |ok| is true if a SET_MEM request succeeded, false otherwise.  This
   * function *must* be called whenever a SET_MEM request is made,
   * regardless of success/failure or special interpretation.
   */
  void reply_set_mem(bool ok);

  /**
   * Reply to the DREQ_SEARCH_MEM request.
   * |found| is true if we found the searched-for bytes starting at address
   * |addr|.
   */
  void reply_search_mem(bool found, remote_ptr<void> addr);

  /**
   * Reply to the DREQ_GET_OFFSETS request.
   */
  void reply_get_offsets(/* TODO */);

  /**
   * Send |value| back to the debugger host.  |value| may be undefined.
   */
  void reply_get_reg(const GdbRegisterValue& value);

  /**
   * Send |file| back to the debugger host.  |file| may contain
   * undefined register values.
   */
  void reply_get_regs(const GdbRegisterFile& file);

  /**
   * Pass |ok = true| iff the requested register was successfully set.
   */
  void reply_set_reg(bool ok);

  /**
   * Reply to the DREQ_GET_STOP_REASON request.
   */
  void reply_get_stop_reason(GdbThreadId which, int sig);

  /**
   * |threads| contains the list of live threads, of which there are
   * |len|.
   */
  void reply_get_thread_list(const std::vector<GdbThreadId>& threads);

  /**
   * |ok| is true if the request was successfully applied, false if
   * not.
   */
  void reply_watchpoint_request(bool ok);

  /**
   * DREQ_DETACH was processed.
   *
   * There's no functional reason to reply to the detach request.
   * However, some versions of gdb expect a response and time out
   * awaiting it, wasting developer time.
   */
  void reply_detach();

  /**
   * Pass the siginfo_t and its size (as requested by the debugger) in
   * |si_bytes| and |num_bytes| if successfully read.  Otherwise pass
   * |si_bytes = nullptr|.
   */
  void reply_read_siginfo(const std::vector<uint8_t>& si_bytes);
  /**
   * Not yet implemented, but call this after a WRITE_SIGINFO request
   * anyway.
   */
  void reply_write_siginfo(/* TODO*/);

  /**
   * Send a manual text response to a rr cmd (maintenance) packet.
   */
  void reply_rr_cmd(const std::string& text);

  /**
   * Create a checkpoint of the given Session with the given id. Delete the
   * existing checkpoint with that id if there is one.
   */
  void created_checkpoint(ReplaySession::shr_ptr& checkpoint,
                          int checkpoint_id);

  /**
   * Delete the checkpoint with the given id. Silently fail if the checkpoint
   * does not exist.
   */
  void delete_checkpoint(int checkpoint_id);

  /**
   * Get the checkpoint with the given id. Return null if not found.
   */
  ReplaySession::shr_ptr get_checkpoint(int checkpoint_id);

  /**
   * Return true if there's a new packet to be read/process (whether
   * incomplete or not), and false if there isn't one.
   */
  bool sniff_packet();

  const Features& features() { return features_; }

private:
  GdbConnection(pid_t tgid, const Features& features);

  /**
   * Wait for a debugger client to connect to |dbg|'s socket.  Blocks
   * indefinitely.
   */
  void await_debugger(ScopedFd& listen_fd);

  /**
   * read() incoming data exactly one time, successfully.  May block.
   */
  void read_data_once();
  /**
   * Send all pending output to gdb.  May block.
   */
  void write_flush();
  void write_data_raw(const uint8_t* data, ssize_t len);
  void write_hex(unsigned long hex);
  void write_packet_bytes(const uint8_t* data, size_t num_bytes);
  void write_packet(const char* data);
  void write_binary_packet(const char* pfx, const uint8_t* data,
                           ssize_t num_bytes);
  void write_hex_bytes_packet(const uint8_t* bytes, size_t len);
  /**
   * Consume bytes in the input buffer until start-of-packet ('$') or
   * the interrupt character is seen.  Does not block.  Return true if
   * seen, false if not.
   */
  bool skip_to_packet_start();
  /**
   * Block until the sequence of bytes
   *
   *    "[^$]*\$[^#]*#.*"
   *
   * has been read from the client fd.  This is one (or more) gdb
   * packet(s).
   */
  void read_packet();
  /**
   * Return true if we need to do something in a debugger request,
   * false if we already handled the packet internally.
   */
  bool xfer(const char* name, char* args);
  /**
   * Return true if we need to do something in a debugger request,
   * false if we already handled the packet internally.
   */
  bool query(char* payload);
  /**
   * Return true if we need to do something in a debugger request,
   * false if we already handled the packet internally.
   */
  bool set_var(char* payload);
  /**
   * Return true if we need to do something in a debugger request,
   * false if we already handled the packet internally.
   */
  bool process_vpacket(char* payload);
  /**
   * Return true if we need to do something in a debugger request,
   * false if we already handled the packet internally.
   */
  bool process_bpacket(char* payload);
  /**
   * Return true if we need to do something in a debugger request,
   * false if we already handled the packet internally.
   */
  bool process_packet();
  void consume_request();
  void send_stop_reply_packet(GdbThreadId thread, int sig,
                              uintptr_t watch_addr = 0);

  // Current request to be processed.
  GdbRequest req;
  // Thread to be resumed.
  GdbThreadId resume_thread;
  // Thread for get/set requests.
  GdbThreadId query_thread;
  // gdb and rr don't work well together in multi-process and
  // multi-exe-image debugging scenarios, so we pretend only
  // this task group exists when interfacing with gdb
  pid_t tgid;
  // true when "no-ack mode" enabled, in which we don't have
  // to send ack packets back to gdb.  This is a huge perf win.
  bool no_ack;
  ScopedFd sock_fd;
  /* XXX probably need to dynamically size these */
  uint8_t inbuf[32768];  /* buffered input from gdb */
  ssize_t inlen;         /* length of valid data */
  ssize_t packetend;     /* index of '#' character */
  uint8_t outbuf[32768]; /* buffered output for gdb */
  ssize_t outlen;
  Features features_;
};

#endif /* RR_GDB_CONNECTION_H_ */
