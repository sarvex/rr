/* -*- Mode: C++; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

#ifndef RR_EXTRA_REGISTERS_H_
#define RR_EXTRA_REGISTERS_H_

#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "GdbRegister.h"
#include "kernel_abi.h"

/**
 * An ExtraRegisters object contains values for all user-space-visible
 * registers other than those in Registers.
 *
 * Task is responsible for creating meaningful values of this class.
 *
 * The only reason this class has an arch() is to enable us to
 * interpret GdbRegister.
 */
class ExtraRegisters {
public:
  // Create empty (uninitialized/unknown registers) value
  ExtraRegisters(SupportedArch arch = SupportedArch(-1))
      : format_(NONE), arch_(arch) {}

  /**
   * On a x86 64-bit kernel, these structures are initialized by an XSAVE64 or
   * FXSAVE64.
   * On a x86 32-bit kernel, they are initialized by an XSAVE or FXSAVE.
   *
   * The layouts are basically the same in the first 512 bytes --- an
   * FXSAVE(64) area. The differences are:
   * -- On a 64-bit kernel, registers XMM8-XMM15 are saved, but on a 32-bit
   * kernel they are not (that space is reserved).
   * -- On a 64-bit kernel, bytes 8-15 store a 64-bit "FPU IP" address,
   * but on a 32-bit kernel they store "FPU IP/CS". Likewise,
   * bytes 16-23 store "FPU DP" or "FPU DP/DS".
   * We basically ignore these differences. If gdb requests 32-bit-specific
   * registers, we return them, assuming that the data there is valid.
   *
   * XSAVE/XSAVE64 have extra information after the first 512 bytes, which we
   * currently save and restore but do not otherwise use. If the data record
   * has more than 512 bytes then it's an XSAVE(64) area, otherwise it's just
   * the FXSAVE(64) area.
   */
  enum Format { NONE, XSAVE };

  // Set values from raw data
  void set_to_raw_data(Format format, std::vector<uint8_t>& consume_data) {
    format_ = format;
    std::swap(data_, consume_data);
  }
  void set_arch(SupportedArch a) { arch_ = a; }

  Format format() const { return format_; }
  SupportedArch arch() const { return arch_; }
  const std::vector<uint8_t> data() const { return data_; }
  int data_size() const { return data_.size(); }
  const uint8_t* data_bytes() const { return data_.data(); }
  bool empty() const { return data_.empty(); }

  /**
   * Like |Registers::read_register()|, except attempts to read
   * the value of an "extra register" (floating point / vector).
   */
  size_t read_register(uint8_t* buf, GdbRegister regno, bool* defined) const;

  /**
   * Get a user_fpregs_struct for a particular Arch from these ExtraRegisters.
   */
  std::vector<uint8_t> get_user_fpregs_struct(SupportedArch arch) const;

  /**
   * Get a user_fpxregs_struct for from these ExtraRegisters.
   */
  rr::X86Arch::user_fpxregs_struct get_user_fpxregs_struct() const;

private:
  friend class Task;

  Format format_;
  SupportedArch arch_;
  std::vector<uint8_t> data_;
};

#endif /* RR_EXTRA_REGISTERS_H_ */
