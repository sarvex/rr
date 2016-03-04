/* -*- Mode: C++; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

#ifndef RR_REGISTERS_H_
#define RR_REGISTERS_H_

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/user.h>

#include "GdbRegister.h"
#include "kernel_abi.h"
#include "kernel_supplement.h"
#include "remote_ptr.h"
#include "remote_code_ptr.h"

class Task;

enum MismatchBehavior {
  EXPECT_MISMATCHES = 0,
  LOG_MISMATCHES,
  BAIL_ON_MISMATCH
};

const uintptr_t X86_RESERVED_FLAG = 1 << 1;
const uintptr_t X86_TF_FLAG = 1 << 8;
const uintptr_t X86_IF_FLAG = 1 << 9;
const uintptr_t X86_DF_FLAG = 1 << 10;
const uintptr_t X86_RF_FLAG = 1 << 16;
const uintptr_t X86_ID_FLAG = 1 << 21;

/**
 * A Registers object contains values for all general-purpose registers.
 * These must include all registers used to pass syscall parameters and return
 * syscall results.
 *
 * When reading register values, be sure to cast the result to the correct
 * type according to the kernel docs. E.g. int values should be cast
 * to int explicitly (or implicitly, by assigning to an int-typed variable),
 * size_t should be cast to size_t, etc. If the type is signed, call the
 * _signed getter. This ensures that when building rr 64-bit we will use the
 * right number of register bits whether the tracee is 32-bit or 64-bit, and
 * get sign-extension right.
 *
 * We have different register sets for different architectures. To ensure a
 * trace can be dumped/processed by an rr build on any platform, we allow
 * Registers to contain registers for any architecture. So we store them
 * in a union of Arch::user_regs_structs for each known Arch.
 */
class Registers {
public:
  enum { MAX_SIZE = 16 };

  Registers(SupportedArch a = SupportedArch(-1)) : arch_(a) {
    memset(&u, 0, sizeof(u));
  }

  SupportedArch arch() const { return arch_; }

  void set_arch(SupportedArch a) { arch_ = a; }

  /**
   * Copy a user_regs_struct into these Registers. If the tracee architecture
   * is not rr's native architecture, then it must be a 32-bit tracee with a
   * 64-bit rr. In that case the user_regs_struct is 64-bit and we extract
   * the 32-bit register values from it into u.x86regs.
   * It's invalid to call this when the Registers' arch is 64-bit and the
   * rr build is 32-bit, or when the Registers' arch is completely different
   * to the rr build (e.g. ARM vs x86).
   */
  void set_from_ptrace(const struct user_regs_struct& ptrace_regs);
  /**
   * Get a user_regs_struct from these Registers. If the tracee architecture
   * is not rr's native architecture, then it must be a 32-bit tracee with a
   * 64-bit rr. In that case the user_regs_struct is 64-bit and we copy
   * the 32-bit register values from u.x86regs into it.
   * It's invalid to call this when the Registers' arch is 64-bit and the
   * rr build is 32-bit, or when the Registers' arch is completely different
   * to the rr build (e.g. ARM vs x86).
   */
  struct user_regs_struct get_ptrace() const;
  /**
   * Get a user_regs_struct for a particular Arch from these Registers.
   * It's invalid to call this when 'arch' is 64-bit and the
   * rr build is 32-bit, or when the Registers' arch is completely different
   * to the rr build (e.g. ARM vs x86).
   */
  std::vector<uint8_t> get_ptrace_for_arch(SupportedArch arch) const;

#define RR_GET_REG(x86case, x64case)                                           \
  (arch() == x86 ? (uint32_t)u.x86regs.x86case                                 \
                 : arch() == x86_64                                            \
                       ? u.x64regs.x64case                                     \
                       : (assert(0 && "unknown architecture"), uintptr_t(-1)))
#define RR_GET_REG_SIGNED(x86case, x64case)                                    \
  (arch() == x86 ? u.x86regs.x86case                                           \
                 : arch() == x86_64                                            \
                       ? u.x64regs.x64case                                     \
                       : (assert(0 && "unknown architecture"), uintptr_t(-1)))
#define RR_SET_REG(x86case, x64case, value)                                    \
  switch (arch()) {                                                            \
    case x86:                                                                  \
      u.x86regs.x86case = (value);                                             \
      break;                                                                   \
    case x86_64:                                                               \
      u.x64regs.x64case = (value);                                             \
      break;                                                                   \
    default:                                                                   \
      assert(0 && "unknown architecture");                                     \
  }

  remote_code_ptr ip() const { return RR_GET_REG(eip, rip); }
  void set_ip(remote_code_ptr addr) {
    RR_SET_REG(eip, rip, addr.register_value());
  }
  remote_ptr<void> sp() const { return RR_GET_REG(esp, rsp); }
  void set_sp(remote_ptr<void> addr) { RR_SET_REG(esp, rsp, addr.as_int()); }

  // Access the registers holding system-call numbers, results, and
  // parameters.

  intptr_t syscallno() const { return RR_GET_REG(eax, rax); }
  void set_syscallno(intptr_t syscallno) { RR_SET_REG(eax, rax, syscallno); }

  uintptr_t syscall_result() const { return RR_GET_REG(eax, rax); }
  intptr_t syscall_result_signed() const { return RR_GET_REG_SIGNED(eax, rax); }
  void set_syscall_result(uintptr_t syscall_result) {
    RR_SET_REG(eax, rax, syscall_result);
  }
  template <typename T> void set_syscall_result(remote_ptr<T> syscall_result) {
    RR_SET_REG(eax, rax, syscall_result.as_int());
  }

  /**
   * Returns true if syscall_result() indicates failure.
   */
  bool syscall_failed() const {
    auto result = syscall_result_signed();
    return -ERANGE <= result && result < 0;
  }
  /**
   * Returns true if syscall_result() indicates a syscall restart.
   */
  bool syscall_may_restart() const {
    switch (-syscall_result_signed()) {
      case ERESTART_RESTARTBLOCK:
      case ERESTARTNOINTR:
      case ERESTARTNOHAND:
      case ERESTARTSYS:
        return true;
      default:
        return false;
    }
  }

  /**
   * This pseudo-register holds the system-call number when we get ptrace
   * enter-system-call and exit-system-call events. Setting it changes
   * the system-call executed when resuming after an enter-system-call
   * event.
   */
  intptr_t original_syscallno() const {
    return RR_GET_REG_SIGNED(orig_eax, orig_rax);
  }
  void set_original_syscallno(intptr_t syscallno) {
    RR_SET_REG(orig_eax, orig_rax, syscallno);
  }

  uintptr_t arg1() const { return RR_GET_REG(ebx, rdi); }
  intptr_t arg1_signed() const { return RR_GET_REG_SIGNED(ebx, rdi); }
  void set_arg1(uintptr_t value) { RR_SET_REG(ebx, rdi, value); }
  template <typename T> void set_arg1(remote_ptr<T> value) {
    RR_SET_REG(ebx, rdi, value.as_int());
  }

  uintptr_t arg2() const { return RR_GET_REG(ecx, rsi); }
  intptr_t arg2_signed() const { return RR_GET_REG_SIGNED(ecx, rsi); }
  void set_arg2(uintptr_t value) { RR_SET_REG(ecx, rsi, value); }
  template <typename T> void set_arg2(remote_ptr<T> value) {
    RR_SET_REG(ecx, rsi, value.as_int());
  }

  uintptr_t arg3() const { return RR_GET_REG(edx, rdx); }
  intptr_t arg3_signed() const { return RR_GET_REG_SIGNED(edx, rdx); }
  void set_arg3(uintptr_t value) { RR_SET_REG(edx, rdx, value); }
  template <typename T> void set_arg3(remote_ptr<T> value) {
    RR_SET_REG(edx, rdx, value.as_int());
  }

  uintptr_t arg4() const { return RR_GET_REG(esi, r10); }
  intptr_t arg4_signed() const { return RR_GET_REG_SIGNED(esi, r10); }
  void set_arg4(uintptr_t value) { RR_SET_REG(esi, r10, value); }
  template <typename T> void set_arg4(remote_ptr<T> value) {
    RR_SET_REG(esi, r10, value.as_int());
  }

  uintptr_t arg5() const { return RR_GET_REG(edi, r8); }
  intptr_t arg5_signed() const { return RR_GET_REG_SIGNED(edi, r8); }
  void set_arg5(uintptr_t value) { RR_SET_REG(edi, r8, value); }
  template <typename T> void set_arg5(remote_ptr<T> value) {
    RR_SET_REG(edi, r8, value.as_int());
  }

  uintptr_t arg6() const { return RR_GET_REG(ebp, r9); }
  intptr_t arg6_signed() const { return RR_GET_REG_SIGNED(ebp, r9); }
  void set_arg6(uintptr_t value) { RR_SET_REG(ebp, r9, value); }
  template <typename T> void set_arg6(remote_ptr<T> value) {
    RR_SET_REG(ebp, r9, value.as_int());
  }

  uintptr_t arg(int index) const {
    switch (index) {
      case 1:
        return arg1();
      case 2:
        return arg2();
      case 3:
        return arg3();
      case 4:
        return arg4();
      case 5:
        return arg5();
      case 6:
        return arg6();
      default:
        assert(0 && "Argument index out of range");
        return 0;
    }
  }

  /**
   * Set the register containing syscall argument |Index| to
   * |value|.
   */
  template <int Index, typename T> void set_arg(T value) {
    set_arg(Index, uintptr_t(value));
  }

  template <int Index, typename T> void set_arg(remote_ptr<T> value) {
    set_arg(Index, value.as_int());
  }

  void set_arg(int index, uintptr_t value) {
    switch (index) {
      case 1:
        return set_arg1(value);
      case 2:
        return set_arg2(value);
      case 3:
        return set_arg3(value);
      case 4:
        return set_arg4(value);
      case 5:
        return set_arg5(value);
      case 6:
        return set_arg6(value);
      default:
        assert(0 && "Argument index out of range");
    }
  }

  // Some X86-specific stuff follows. Use of these accessors should be guarded
  // by an architecture test.
  /**
   * Set the output registers of the |rdtsc| instruction.
   */
  void set_rdtsc_output(uint64_t value) {
    RR_SET_REG(eax, rax, value & 0xffffffff);
    RR_SET_REG(edx, rdx, value >> 32);
  }

  uintptr_t r11() const {
    assert(arch() == x86_64);
    return u.x64regs.r11;
  }
  void set_r11(uintptr_t value) {
    assert(arch() == x86_64);
    u.x64regs.r11 = value;
  }

  uintptr_t di() const { return RR_GET_REG(edi, rdi); }
  void set_di(uintptr_t value) { RR_SET_REG(edi, rdi, value); }

  uintptr_t si() const { return RR_GET_REG(esi, rsi); }
  void set_si(uintptr_t value) { RR_SET_REG(esi, rsi, value); }

  uintptr_t cx() const { return RR_GET_REG(ecx, rcx); }
  void set_cx(uintptr_t value) { RR_SET_REG(ecx, rcx, value); }

  uintptr_t bp() const { return RR_GET_REG(ebp, rbp); }

  uintptr_t flags() const;
  void set_flags(uintptr_t value);
  bool singlestep_flag() { return flags() & X86_TF_FLAG; }
  void clear_singlestep_flag() { set_flags(flags() & ~X86_TF_FLAG); }
  bool df_flag() const { return flags() & X86_DF_FLAG; }

  // End of X86-specific stuff

  void print_register_file(FILE* f) const;
  void print_register_file_compact(FILE* f) const;
  void print_register_file_for_trace(FILE* f) const;
  void print_register_file_for_trace_raw(FILE* f) const;

  /**
   * Return true if |reg1| matches |reg2|.  Passing EXPECT_MISMATCHES
   * indicates that the caller is using this as a general register
   * compare and nothing special should be done if the register files
   * mismatch.  Passing LOG_MISMATCHES will log the registers that don't
   * match.  Passing BAIL_ON_MISMATCH will additionally abort on
   * mismatch.
   */
  static bool compare_register_files(Task* t, const char* name1,
                                     const Registers& reg1, const char* name2,
                                     const Registers& reg2,
                                     MismatchBehavior mismatch_behavior);

  bool matches(const Registers& other) const {
    return compare_register_files(nullptr, nullptr, *this, nullptr, other,
                                  EXPECT_MISMATCHES);
  }

  /**
   * Return the total number of registers for this target.
   */
  size_t total_registers() const;

  // TODO: refactor me to use the GdbRegisterValue helper from
  // GdbConnection.h.

  /**
   * Write the value for register |regno| into |buf|, which should
   * be large enough to hold any register supported by the target.
   * Return the size of the register in bytes and set |defined| to
   * indicate whether a useful value has been written to |buf|.
   */
  size_t read_register(uint8_t* buf, GdbRegister regno, bool* defined) const;

  /**
   * Write the value for register |offset| into |buf|, which should
   * be large enough to hold any register supported by the target.
   * Return the size of the register in bytes and set |defined| to
   * indicate whether a useful value has been written to |buf|.
   * |offset| is the offset of the register within a user_regs_struct.
   */
  size_t read_register_by_user_offset(uint8_t* buf, uintptr_t offset,
                                      bool* defined) const;

  /**
   * Update the registe named |reg_name| to |value| with
   * |value_size| number of bytes.
   */
  void write_register(GdbRegister reg_name, const uint8_t* value,
                      size_t value_size);

private:
  template <typename Arch>
  void print_register_file_arch(FILE* f, const char* formats[]) const;

  enum TraceStyle {
    Annotated,
    Raw,
  };

  template <typename Arch>
  void print_register_file_for_trace_arch(FILE* f, TraceStyle style,
                                          const char* formats[]) const;

  template <typename Arch>
  static bool compare_registers_core(const char* name1, const Registers& reg1,
                                     const char* name2, const Registers& reg2,
                                     MismatchBehavior mismatch_behavior);

  template <typename Arch>
  static bool compare_registers_arch(const char* name1, const Registers& reg1,
                                     const char* name2, const Registers& reg2,
                                     MismatchBehavior mismatch_behavior);

  static bool compare_register_files_internal(
      const char* name1, const Registers& reg1, const char* name2,
      const Registers& reg2, MismatchBehavior mismatch_behavior);

  template <typename Arch>
  size_t read_register_arch(uint8_t* buf, GdbRegister regno,
                            bool* defined) const;

  template <typename Arch>
  size_t read_register_by_user_offset_arch(uint8_t* buf, uintptr_t offset,
                                           bool* defined) const;

  template <typename Arch>
  void write_register_arch(GdbRegister regno, const uint8_t* value,
                           size_t value_size);

  template <typename Arch> size_t total_registers_arch() const;

  union AllRegisters {
    rr::X86Arch::user_regs_struct x86regs;
    rr::X64Arch::user_regs_struct x64regs;
  } u;
  SupportedArch arch_;
};

std::ostream& operator<<(std::ostream& stream, const Registers& r);

#endif /* RR_REGISTERS_H_ */
