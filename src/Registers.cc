/* -*- Mode: C++; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

//#define DEBUGTAG "registers"

#include "Registers.h"

#include <array>
#include <initializer_list>
#include <utility>

#include <assert.h>
#include <string.h>

#include "log.h"
#include "task.h"

using namespace rr;
using namespace std;

struct RegisterValue {
  // The name of this register.
  const char* name;
  // The offsetof the register in user_regs_struct.
  size_t offset;
  // The size of the register.  0 means we cannot read it.
  size_t nbytes;
  // Mask to be applied to register values prior to comparing them.  Will
  // typically be ((1 << nbytes) - 1), but some registers may have special
  // comparison semantics.
  uint64_t comparison_mask;

  constexpr RegisterValue()
      : name(nullptr), offset(0), nbytes(0), comparison_mask(0) {}

  RegisterValue(const char* name_, size_t offset_, size_t nbytes_)
      : name(name_), offset(offset_), nbytes(nbytes_) {
    comparison_mask = mask_for_nbytes(nbytes_);
  }

  RegisterValue(const char* name_, size_t offset_, size_t nbytes_,
                uint64_t comparison_mask_)
      : name(name_),
        offset(offset_),
        nbytes(nbytes_),
        comparison_mask(comparison_mask_) {
    // Ensure no bits are set outside of the register's bitwidth.
    assert((comparison_mask_ & ~mask_for_nbytes(nbytes_)) == 0);
  }
  // Returns a pointer to the register in |regs| represented by |offset|.
  // |regs| is assumed to be a pointer to the user_struct_regs for the
  // appropriate architecture.
  void* pointer_into(void* regs) { return static_cast<char*>(regs) + offset; }

  const void* pointer_into(const void* regs) {
    return static_cast<const char*>(regs) + offset;
  }

  static uint64_t mask_for_nbytes(size_t nbytes) {
    assert(nbytes <= sizeof(comparison_mask));
    return ((nbytes == sizeof(comparison_mask)) ? uint64_t(0)
                                                : (uint64_t(1) << nbytes * 8)) -
           1;
  }
};

typedef std::pair<size_t, RegisterValue> RegisterInit;

template <size_t N> struct RegisterTable : std::array<RegisterValue, N> {
  RegisterTable(std::initializer_list<RegisterInit> list) {
    for (auto& ri : list) {
      (*this)[ri.first] = ri.second;
    }
  }
};

template <typename T> struct RegisterInfo;

template <> struct RegisterInfo<rr::X86Arch> {
  static bool ignore_undefined_register(GdbRegister regno) {
    return regno == DREG_FOSEG || regno == DREG_MXCSR;
  }
  static const size_t num_registers = DREG_NUM_LINUX_I386;
  typedef RegisterTable<num_registers> Table;
  static Table registers;
  static RegisterValue non_gdb_registers[0];
};

template <> struct RegisterInfo<rr::X64Arch> {
  static bool ignore_undefined_register(GdbRegister regno) {
    return regno == DREG_64_FOSEG || regno == DREG_64_MXCSR;
  }
  static const size_t num_registers = DREG_NUM_LINUX_X86_64;
  typedef RegisterTable<num_registers> Table;
  static Table registers;
  static RegisterValue non_gdb_registers[2];
};

#define RV_ARCH(gdb_suffix, name, arch, extra_ctor_args)                       \
  RegisterInit(DREG_##gdb_suffix,                                              \
               RegisterValue(#name, offsetof(arch::user_regs_struct, name),    \
                             sizeof(((arch::user_regs_struct*)0)->name)        \
                                 extra_ctor_args))
#define RV_X86(gdb_suffix, name)                                               \
  RV_ARCH(gdb_suffix, name, rr::X86Arch, /* empty */)
#define RV_X64(gdb_suffix, name)                                               \
  RV_ARCH(gdb_suffix, name, rr::X64Arch, /* empty */)
#define COMMA ,
#define RV_X86_WITH_MASK(gdb_suffix, name, comparison_mask)                    \
  RV_ARCH(gdb_suffix, name, rr::X86Arch, COMMA comparison_mask)
#define RV_X64_WITH_MASK(gdb_suffix, name, comparison_mask)                    \
  RV_ARCH(gdb_suffix, name, rr::X64Arch, COMMA comparison_mask)

const uint64_t deterministic_eflags_mask = ~uint32_t(
    /* The following are eflags that have been observed to be non-deterministic
       in practice.  We need to mask them off when comparing registers to
       prevent replay from diverging.  */
    /* The linux kernel has been observed to report this as zero in some
       states during system calls.  It always seems to be 1 during user-space
       execution so we should be able to ignore it.  */
    X86_RESERVED_FLAG |
    /* This is usually set but we have observed cases where it's clear. It
     * shouldn't be modifiable by user space so we don't know why it would
     * change.
     */
    X86_IF_FLAG |
    /* According to http://www.logix.cz/michal/doc/i386/chp04-01.htm:
          The RF flag temporarily disables debug exceptions so that an
         instruction can be restarted after a debug exception without
         immediately causing another debug exception.  Refer to Chapter 12
         for details.
        Chapter 12 isn't particularly clear on the point, but the flag appears
       to be set by |int3| exceptions.
        This divergence has been observed when continuing a tracee to an
       execution target by setting an |int3| breakpoint, which isn't used
       during recording.  No single-stepping was used during the recording
       either.  */
    X86_RF_FLAG |
    /* It is no longer known why this bit is ignored.  */
    X86_ID_FLAG);

RegisterInfo<rr::X86Arch>::Table RegisterInfo<rr::X86Arch>::registers = {
  RV_X86(EAX, eax), RV_X86(ECX, ecx), RV_X86(EDX, edx), RV_X86(EBX, ebx),
  RV_X86(ESP, esp), RV_X86(EBP, ebp), RV_X86(ESI, esi), RV_X86(EDI, edi),
  RV_X86(EIP, eip), RV_X86_WITH_MASK(EFLAGS, eflags, deterministic_eflags_mask),
  RV_X86_WITH_MASK(CS, xcs, 0), RV_X86_WITH_MASK(SS, xss, 0),
  RV_X86_WITH_MASK(DS, xds, 0), RV_X86_WITH_MASK(ES, xes, 0), RV_X86(FS, xfs),
  RV_X86(GS, xgs),
  // The comparison for this is handled specially elsewhere.
  RV_X86_WITH_MASK(ORIG_EAX, orig_eax, 0),
};

RegisterValue RegisterInfo<rr::X86Arch>::non_gdb_registers[0] = {};

RegisterInfo<rr::X64Arch>::Table RegisterInfo<rr::X64Arch>::registers = {
  RV_X64(RAX, rax), RV_X64(RCX, rcx), RV_X64(RDX, rdx), RV_X64(RBX, rbx),
  RV_X64_WITH_MASK(RSP, rsp, 0), RV_X64(RBP, rbp), RV_X64(RSI, rsi),
  RV_X64(RDI, rdi), RV_X64(R8, r8), RV_X64(R9, r9), RV_X64(R10, r10),
  RV_X64(R11, r11), RV_X64(R12, r12), RV_X64(R13, r13), RV_X64(R14, r14),
  RV_X64(R15, r15), RV_X64(RIP, rip),
  RV_X64_WITH_MASK(64_EFLAGS, eflags, deterministic_eflags_mask),
  RV_X64_WITH_MASK(64_CS, cs, 0), RV_X64_WITH_MASK(64_SS, ss, 0),
  RV_X64_WITH_MASK(64_DS, ds, 0), RV_X64_WITH_MASK(64_ES, es, 0),
  RV_X64(64_FS, fs), RV_X64(64_GS, gs),
  // The comparison for this is handled specially
  // elsewhere.
  RV_X64_WITH_MASK(ORIG_RAX, orig_rax, 0),
};

RegisterValue RegisterInfo<rr::X64Arch>::non_gdb_registers[2] = {
  { "fs_base", offsetof(rr::X64Arch::user_regs_struct, fs_base), 8,
    RegisterValue::mask_for_nbytes(8) },
  { "gs_base", offsetof(rr::X64Arch::user_regs_struct, gs_base), 8,
    RegisterValue::mask_for_nbytes(8) }
};

#undef RV_X64
#undef RV_X86
#undef RV_ARCH

// 32-bit format, 64-bit format for all of these.
// format_index in RegisterPrinting depends on the ordering here.
static const char* hex_format[] = { "%" PRIx32, "%" PRIx64 };
static const char* hex_format_leading_0x[] = { "0x%" PRIx32, "0x%" PRIx64 };
// static const char* decimal_format[] = { "%" PRId32, "%" PRId64 };

template <size_t nbytes> struct RegisterPrinting;

template <> struct RegisterPrinting<4> {
  typedef uint32_t type;
  static const size_t format_index = 0;
};

template <> struct RegisterPrinting<8> {
  typedef uint64_t type;
  static const size_t format_index = 1;
};

template <size_t nbytes>
void print_single_register(FILE* f, const char* name, const void* register_ptr,
                           const char* formats[]) {
  typename RegisterPrinting<nbytes>::type val;
  memcpy(&val, register_ptr, nbytes);
  if (name) {
    fprintf(f, "%s:", name);
  } else {
    fprintf(f, " ");
  }
  fprintf(f, formats[RegisterPrinting<nbytes>::format_index], val);
}

template <typename Arch>
void Registers::print_register_file_arch(FILE* f, const char* formats[]) const {
  fprintf(f, "Printing register file:\n");
  const void* user_regs = &u;
  for (auto& rv : RegisterInfo<Arch>::registers) {
    if (rv.nbytes == 0) {
      continue;
    }
    switch (rv.nbytes) {
      case 8:
        print_single_register<8>(f, rv.name, rv.pointer_into(user_regs),
                                 formats);
        break;
      case 4:
        print_single_register<4>(f, rv.name, rv.pointer_into(user_regs),
                                 formats);
        break;
      default:
        assert(0 && "bad register size");
    }
    fprintf(f, "\n");
  }
  fprintf(f, "\n");
}

void Registers::print_register_file(FILE* f) const {
  RR_ARCH_FUNCTION(print_register_file_arch, arch(), f, hex_format);
}

template <typename Arch>
void Registers::print_register_file_for_trace_arch(
    FILE* f, TraceStyle style, const char* formats[]) const {
  const void* user_regs = &u;
  for (auto& rv : RegisterInfo<Arch>::registers) {
    if (rv.nbytes == 0) {
      continue;
    }

    fprintf(f, " ");
    const char* name = (style == Annotated ? rv.name : nullptr);

    switch (rv.nbytes) {
      case 8:
        print_single_register<8>(f, name, rv.pointer_into(user_regs), formats);
        break;
      case 4:
        print_single_register<4>(f, name, rv.pointer_into(user_regs), formats);
        break;
      default:
        assert(0 && "bad register size");
    }
  }

  for (auto& rv : RegisterInfo<Arch>::non_gdb_registers) {
    fprintf(f, " ");
    const char* name = (style == Annotated ? rv.name : nullptr);

    switch (rv.nbytes) {
      case 8:
        print_single_register<8>(f, name, rv.pointer_into(user_regs), formats);
        break;
      case 4:
        print_single_register<4>(f, name, rv.pointer_into(user_regs), formats);
        break;
      default:
        assert(0 && "bad register size");
    }
  }
}

void Registers::print_register_file_compact(FILE* f) const {
  RR_ARCH_FUNCTION(print_register_file_for_trace_arch, arch(), f, Annotated,
                   hex_format);
}

void Registers::print_register_file_for_trace(FILE* f) const {
  RR_ARCH_FUNCTION(print_register_file_for_trace_arch, arch(), f, Annotated,
                   hex_format_leading_0x);
}

void Registers::print_register_file_for_trace_raw(FILE* f) const {
  fprintf(f, " %d %d %d %d %d %d %d"
             " %d %d %d %d",
          u.x86regs.eax, u.x86regs.ebx, u.x86regs.ecx, u.x86regs.edx,
          u.x86regs.esi, u.x86regs.edi, u.x86regs.ebp, u.x86regs.orig_eax,
          u.x86regs.esp, u.x86regs.eip, u.x86regs.eflags);
}

static void maybe_print_reg_mismatch(MismatchBehavior mismatch_behavior,
                                     const char* regname, const char* label1,
                                     uint64_t val1, const char* label2,
                                     uint64_t val2) {
  if (mismatch_behavior >= BAIL_ON_MISMATCH) {
    LOG(error) << regname << " " << HEX(val1) << " != " << HEX(val2) << " ("
               << label1 << " vs. " << label2 << ")";
  } else if (mismatch_behavior >= LOG_MISMATCHES) {
    LOG(info) << regname << " " << HEX(val1) << " != " << HEX(val2) << " ("
              << label1 << " vs. " << label2 << ")";
  }
}

template <typename Arch>
bool Registers::compare_registers_core(const char* name1, const Registers& reg1,
                                       const char* name2, const Registers& reg2,
                                       MismatchBehavior mismatch_behavior) {
  bool match = true;

  for (auto& rv : RegisterInfo<Arch>::registers) {
    if (rv.nbytes == 0) {
      continue;
    }

    // Disregard registers that will trivially compare equal.
    if (rv.comparison_mask == 0) {
      continue;
    }

    // XXX correct but oddly displayed for big-endian processors.
    uint64_t val1 = 0, val2 = 0;
    memcpy(&val1, rv.pointer_into(&reg1.u), rv.nbytes);
    memcpy(&val2, rv.pointer_into(&reg2.u), rv.nbytes);

    if ((val1 ^ val2) & rv.comparison_mask) {
      maybe_print_reg_mismatch(mismatch_behavior, rv.name, name1, val1, name2,
                               val2);
      match = false;
    }
  }

  return match;
}

// A handy macro for compare_registers_arch specializations.
#define REGCMP(user_regs, _reg)                                                \
  do {                                                                         \
    if (reg1.user_regs._reg != reg2.user_regs._reg) {                          \
      maybe_print_reg_mismatch(mismatch_behavior, #_reg, name1,                \
                               reg1.user_regs._reg, name2,                     \
                               reg2.user_regs._reg);                           \
      match = false;                                                           \
    }                                                                          \
  } while (0)
#define X86_REGCMP(_reg) REGCMP(u.x86regs, _reg)
#define X64_REGCMP(_reg) REGCMP(u.x64regs, _reg)

// A wrapper around compare_registers_core so registers requiring special
// processing can be handled via template specialization.
template <typename Arch>
/* static */ bool Registers::compare_registers_arch(
    const char* name1, const Registers& reg1, const char* name2,
    const Registers& reg2, MismatchBehavior mismatch_behavior) {
  // Default behavior.
  return compare_registers_core<Arch>(name1, reg1, name2, reg2,
                                      mismatch_behavior);
}

template <>
/* static */ bool Registers::compare_registers_arch<rr::X86Arch>(
    const char* name1, const Registers& reg1, const char* name2,
    const Registers& reg2, MismatchBehavior mismatch_behavior) {
  bool match = compare_registers_core<rr::X86Arch>(name1, reg1, name2, reg2,
                                                   mismatch_behavior);
  /* Negative orig_eax values, observed at SCHED events and signals,
     seemingly can vary between recording and replay on some kernels
     (e.g. Linux ubuntu 3.13.0-24-generic). They probably reflect
     signals sent or something like that.
  */
  if (reg1.u.x86regs.orig_eax >= 0 || reg2.u.x86regs.orig_eax >= 0) {
    X86_REGCMP(orig_eax);
  }
  return match;
}

template <>
/* static */ bool Registers::compare_registers_arch<rr::X64Arch>(
    const char* name1, const Registers& reg1, const char* name2,
    const Registers& reg2, MismatchBehavior mismatch_behavior) {
  bool match = compare_registers_core<rr::X64Arch>(name1, reg1, name2, reg2,
                                                   mismatch_behavior);
  // XXX haven't actually observed this to be true on x86-64 yet, but
  // assuming that it follows the x86 behavior.
  if ((intptr_t)reg1.u.x64regs.orig_rax >= 0 ||
      (intptr_t)reg2.u.x64regs.orig_rax >= 0) {
    X64_REGCMP(orig_rax);
  }
  // Check the _upper bits of various registers we defined more conveniently
  // for our gdb support.
  X64_REGCMP(cs_upper);
  X64_REGCMP(ds_upper);
  X64_REGCMP(es_upper);
  X64_REGCMP(fs_upper);
  X64_REGCMP(gs_upper);
  X64_REGCMP(ss_upper);
  X64_REGCMP(eflags_upper);
  return match;
}

/*static*/ bool Registers::compare_register_files_internal(
    const char* name1, const Registers& reg1, const char* name2,
    const Registers& reg2, MismatchBehavior mismatch_behavior) {
  assert(reg1.arch() == reg2.arch());
  RR_ARCH_FUNCTION(compare_registers_arch, reg1.arch(), name1, reg1, name2,
                   reg2, mismatch_behavior);
}

/*static*/ bool Registers::compare_register_files(
    Task* t, const char* name1, const Registers& reg1, const char* name2,
    const Registers& reg2, MismatchBehavior mismatch_behavior) {
  bool bail_error = mismatch_behavior >= BAIL_ON_MISMATCH;
  bool match = compare_register_files_internal(name1, reg1, name2, reg2,
                                               mismatch_behavior);

  ASSERT(t, !bail_error || match)
      << "Fatal register mismatch (ticks/rec:" << t->tick_count() << "/"
      << t->current_trace_frame().ticks() << ")";

  if (match && mismatch_behavior == LOG_MISMATCHES) {
    LOG(info) << "(register files are the same for " << name1 << " and "
              << name2 << ")";
  }

  return match;
}

template <typename Arch>
size_t Registers::read_register_arch(uint8_t* buf, GdbRegister regno,
                                     bool* defined) const {
  assert(regno < total_registers());
  // Make sure these two definitions don't get out of sync.
  assert(array_length(RegisterInfo<Arch>::registers) == total_registers());

  RegisterValue& rv = RegisterInfo<Arch>::registers[regno];
  if (rv.nbytes == 0) {
    *defined = false;
  } else {
    *defined = true;
    memcpy(buf, rv.pointer_into(&u), rv.nbytes);
  }

  return rv.nbytes;
}

size_t Registers::read_register(uint8_t* buf, GdbRegister regno,
                                bool* defined) const {
  RR_ARCH_FUNCTION(read_register_arch, arch(), buf, regno, defined);
}

template <typename Arch>
size_t Registers::read_register_by_user_offset_arch(uint8_t* buf,
                                                    uintptr_t offset,
                                                    bool* defined) const {
  for (size_t regno = 0; regno < RegisterInfo<Arch>::num_registers; ++regno) {
    RegisterValue& rv = RegisterInfo<Arch>::registers[regno];
    if (rv.offset == offset) {
      return read_register_arch<Arch>(buf, GdbRegister(regno), defined);
    }
  }

  *defined = false;
  return 0;
}

size_t Registers::read_register_by_user_offset(uint8_t* buf, uintptr_t offset,
                                               bool* defined) const {
  RR_ARCH_FUNCTION(read_register_by_user_offset_arch, arch(), buf, offset,
                   defined);
}

template <typename Arch>
void Registers::write_register_arch(GdbRegister regno, const uint8_t* value,
                                    size_t value_size) {
  RegisterValue& rv = RegisterInfo<Arch>::registers[regno];

  if (rv.nbytes == 0) {
    // TODO: can we get away with not writing these?
    if (RegisterInfo<Arch>::ignore_undefined_register(regno)) {
      return;
    }
    LOG(warn) << "Unhandled register name " << regno;
  } else {
    assert(value_size == rv.nbytes);
    memcpy(rv.pointer_into(&u), value, value_size);
  }
}

void Registers::write_register(GdbRegister regno, const uint8_t* value,
                               size_t value_size) {
  RR_ARCH_FUNCTION(write_register_arch, arch(), regno, value, value_size);
}

template <typename Arch> size_t Registers::total_registers_arch() const {
  return RegisterInfo<Arch>::num_registers;
}

size_t Registers::total_registers() const {
  RR_ARCH_FUNCTION(total_registers_arch, arch());
}

// In theory it doesn't matter how 32-bit register values are sign extended
// to 64 bits for PTRACE_SETREGS. However:
// -- When setting up a signal handler frame, the kernel does some arithmetic
// on the 64-bit SP value and validates that the result points to writeable
// memory. This validation fails if SP has been sign-extended to point
// outside the 32-bit address space.
// -- Some kernels (e.g. 4.3.3-301.fc23.x86_64) with commmit
// c5c46f59e4e7c1ab244b8d38f2b61d317df90bba have a bug where if you clear
// the upper 32 bits of %rax while in the kernel, syscalls may fail to
// restart. So sign-extension is necessary for %eax in this case. We may as
// well sign-extend %eax in all cases.

typedef void (*NarrowConversion)(int32_t& r32, uint64_t& r64);
typedef void (*SameConversion)(int32_t& r32, uint32_t& r64);
template <NarrowConversion narrow, NarrowConversion narrow_signed,
          SameConversion same>
void convert_x86(X86Arch::user_regs_struct& x86,
                 X64Arch::user_regs_struct& x64) {
  narrow_signed(x86.eax, x64.rax);
  narrow(x86.ebx, x64.rbx);
  narrow(x86.ecx, x64.rcx);
  narrow(x86.edx, x64.rdx);
  narrow(x86.esi, x64.rsi);
  narrow(x86.edi, x64.rdi);
  narrow(x86.esp, x64.rsp);
  narrow(x86.ebp, x64.rbp);
  narrow(x86.eip, x64.rip);
  narrow(x86.orig_eax, x64.orig_rax);
  same(x86.eflags, x64.eflags);
  same(x86.xcs, x64.cs);
  same(x86.xds, x64.ds);
  same(x86.xes, x64.es);
  same(x86.xfs, x64.fs);
  same(x86.xgs, x64.gs);
  same(x86.xss, x64.ss);
}

void to_x86_narrow(int32_t& r32, uint64_t& r64) { r32 = r64; }
void to_x86_same(int32_t& r32, uint32_t& r64) { r32 = r64; }
void from_x86_narrow(int32_t& r32, uint64_t& r64) { r64 = (uint32_t)r32; }
void from_x86_narrow_signed(int32_t& r32, uint64_t& r64) { r64 = (int64_t)r32; }
void from_x86_same(int32_t& r32, uint32_t& r64) { r64 = r32; }

void Registers::set_from_ptrace(const struct user_regs_struct& ptrace_regs) {
  if (arch() == NativeArch::arch()) {
    memcpy(&u, &ptrace_regs, sizeof(ptrace_regs));
    return;
  }

  assert(arch() == x86 && NativeArch::arch() == x86_64);
  convert_x86<to_x86_narrow, to_x86_narrow, to_x86_same>(
      u.x86regs, *reinterpret_cast<X64Arch::user_regs_struct*>(
                     const_cast<struct user_regs_struct*>(&ptrace_regs)));
}

/**
 * Get a user_regs_struct from these Registers. If the tracee architecture
 * is not rr's native architecture, then it must be a 32-bit tracee with a
 * 64-bit rr. In that case the user_regs_struct is 64-bit and we copy
 * the 32-bit register values from u.x86regs into it.
 */
struct user_regs_struct Registers::get_ptrace() const {
  union {
    struct user_regs_struct linux_api;
    struct X64Arch::user_regs_struct x64arch_api;
  } result;
  if (arch() == NativeArch::arch()) {
    memcpy(&result, &u, sizeof(result));
    return result.linux_api;
  }

  assert(arch() == x86 && NativeArch::arch() == x86_64);
  memset(&result, 0, sizeof(result));
  convert_x86<from_x86_narrow, from_x86_narrow_signed, from_x86_same>(
      const_cast<Registers*>(this)->u.x86regs, result.x64arch_api);
  return result.linux_api;
}

vector<uint8_t> Registers::get_ptrace_for_arch(SupportedArch arch) const {
  Registers tmp_regs(arch);
  tmp_regs.set_from_ptrace(get_ptrace());

  vector<uint8_t> result;
  switch (arch) {
    case x86:
      result.resize(sizeof(u.x86regs));
      memcpy(result.data(), &u.x86regs, result.size());
      break;
    case x86_64:
      result.resize(sizeof(u.x64regs));
      memcpy(result.data(), &u.x64regs, result.size());
      break;
    default:
      assert(0 && "Unknown arch");
      break;
  }
  return result;
}

uintptr_t Registers::flags() const {
  switch (arch()) {
    case x86:
      return u.x86regs.eflags;
    case x86_64:
      return u.x64regs.eflags | (uint64_t(u.x64regs.eflags_upper) << 32);
    default:
      assert(0 && "Unknown arch");
      return false;
  }
}

void Registers::set_flags(uintptr_t value) {
  switch (arch()) {
    case x86:
      u.x86regs.eflags = value;
      break;
    case x86_64:
      u.x64regs.eflags = value;
      u.x64regs.eflags_upper = uint64_t(value) >> 32;
      break;
    default:
      assert(0 && "Unknown arch");
      break;
  }
}

ostream& operator<<(ostream& stream, const Registers& r) {
  stream << "{ args:(" << HEX(r.arg1()) << "," << HEX(r.arg2()) << ","
         << HEX(r.arg3()) << "," << HEX(r.arg4()) << "," << HEX(r.arg5()) << ","
         << r.arg6() << ") orig_syscall:" << r.original_syscallno() << " }";
  return stream;
}
