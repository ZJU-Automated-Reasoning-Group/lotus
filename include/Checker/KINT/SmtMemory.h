#pragma once

#include <string>
#include <vector>

#include <z3++.h>

namespace kint {

// A tiny, byte-addressed SMT memory model:
// - address sort: bit-vector (addrBits)
// - value sort:   8-bit bit-vector (byte)
// - load/store of wider integers is implemented via byte concat/split
class SmtMemory {
public:
  SmtMemory(z3::context &ctx, unsigned addrBits);

  unsigned addrBits() const { return m_addrBits; }

  const z3::expr &mem() const { return m_mem; }
  z3::expr &mem() { return m_mem; }

  void push();
  void pop();
  void reset();

  // Conservatively forget all current memory contents (represents unknown side
  // effects).
  void havoc(const std::string &hint = "havoc");

  // Loads/stores raw byte vectors of width (numBytes * 8).
  z3::expr loadBytes(const z3::expr &addr, unsigned numBytes,
                     bool littleEndian = true) const;
  void storeBytes(const z3::expr &addr, const z3::expr &value,
                  unsigned numBytes, bool littleEndian = true);

  // Convenience wrappers for integer-typed loads/stores, where `storeBytes` is
  // the LLVM store size in bytes.
  z3::expr loadInt(const z3::expr &addr, unsigned bitWidth, unsigned storeBytes,
                   bool littleEndian = true) const;
  void storeInt(const z3::expr &addr, const z3::expr &value, unsigned bitWidth,
                unsigned storeSizeBytes, bool littleEndian = true);

  // Helpers for modeling common memory intrinsics when the length is constant.
  // For large lengths, callers should prefer `havoc()` to avoid large
  // constraint sets.
  void memsetBytes(const z3::expr &dst, const z3::expr &byteVal,
                   uint64_t numBytes);
  void memcpyBytes(const z3::expr &dst, const z3::expr &src, uint64_t numBytes);

private:
  z3::expr freshMemory(const std::string &hint) const;
  z3::expr addrAdd(const z3::expr &addr, uint64_t byteOffset) const;

  z3::context &m_ctx;
  unsigned m_addrBits;
  z3::expr m_mem;
  std::vector<z3::expr> m_stack;
};

} // namespace kint
