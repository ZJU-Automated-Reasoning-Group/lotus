#pragma once

#include "Verification/SymAbsAI/Core/FunctionContext.h"

#include <z3++.h>

namespace symabs_ai {
class MemoryModel {
protected:
  const FunctionContext &Fctx_;

private:
  const z3::sort PtrSort_;

public:
  explicit MemoryModel(const FunctionContext &fctx)
      : Fctx_(fctx), PtrSort_(fctx.getZ3().bv_sort(fctx.getPointerSize())) {}

  virtual z3::expr load(z3::expr result, z3::expr mem, z3::expr addr) const = 0;
  virtual z3::expr store(z3::expr pre, z3::expr post, z3::expr addr,
                         z3::expr val) const = 0;
  virtual z3::expr copy(z3::expr mem_pre, z3::expr mem_post) const = 0;
  virtual z3::sort sort() const = 0;
  virtual ~MemoryModel() {}

  virtual z3::sort ptr_sort() const;
  virtual z3::expr allocate(z3::expr, z3::expr, z3::expr, z3::expr) const;
  virtual z3::expr deallocate(z3::expr, z3::expr, z3::expr) const;
  virtual z3::expr getelementptr(z3::expr result, z3::expr ptr,
                                 z3::expr offset) const;
  virtual z3::expr ptrtoint(z3::expr result, z3::expr arg,
                            unsigned int size) const;
  virtual z3::expr inttoptr(z3::expr result, z3::expr arg) const;
  virtual z3::expr make_nullptr() const;

  virtual z3::expr init_memory(z3::expr mm) const;

  virtual z3::expr get_ptr_value(z3::expr x) const { return x; }

  static unique_ptr<MemoryModel> New(const FunctionContext &fctx);
};

namespace memory {
class BlockModel : public MemoryModel {
private:
  z3::sort Sort_;
  z3::sort PtrSort_;
  z3::sort BlockIdSort_;

  z3::func_decl MkMem_;
  z3::func_decl GetNextAlloc_;
  z3::func_decl GetMap_;

  z3::func_decl MkPtr_;
  z3::func_decl GetIdx_;
  z3::func_decl GetBlock_;

public:
  explicit BlockModel(const FunctionContext &fctx);

  virtual ~BlockModel() {}

  virtual z3::expr load(z3::expr, z3::expr, z3::expr) const override {
    return Fctx_.getZ3().bool_val(true);
  }

  virtual z3::expr store(z3::expr pre, z3::expr post, z3::expr,
                         z3::expr) const override {
    return (pre == post);
  }

  virtual z3::expr copy(z3::expr, z3::expr) const override;
  virtual z3::sort sort() const override;
  virtual z3::sort ptr_sort() const override;

  /**
   * For use in InstructionSemantics
   */
  virtual z3::expr allocate(z3::expr mem_before, z3::expr mem_after,
                            z3::expr result, z3::expr size) const override;
  virtual z3::expr deallocate(z3::expr mem_before, z3::expr mem_after,
                              z3::expr ptr) const override;
  virtual z3::expr getelementptr(z3::expr result, z3::expr ptr,
                                 z3::expr offset) const override;
  virtual z3::expr ptrtoint(z3::expr result, z3::expr arg,
                            unsigned int size) const override;
  virtual z3::expr inttoptr(z3::expr result, z3::expr arg) const override;

  /**
   * Returns the SMT representation of a null pointer constant.
   */
  virtual z3::expr make_nullptr() const override;

  /**
   * Returns a z3::expr that represents the actual bv value of the pointer
   * expression x.
   */
  virtual z3::expr get_ptr_value(z3::expr x) const override;

  /**
   * Returns a formula that is true iff p and q are pointers that map to
   * distinct memory regions.
   */
  z3::expr no_alias(z3::expr p, z3::expr q) const;

  /**
   * Returns a formula that is true iff p is a pointer to the beginning of a
   * valid memory region.
   */
  z3::expr valid_region(z3::expr mem, z3::expr p) const;

  /**
   * Returns a bv formula representing the size of memory region from p to the
   * end of the block pointed to by p.
   */
  z3::expr region_size(z3::expr mem, z3::expr p) const;

  /**
   * Initialize the memory model.
   */
  virtual z3::expr init_memory(z3::expr mm) const override;
};

class NoMemory : public MemoryModel {
private:
  z3::expr True_;

public:
  explicit NoMemory(const FunctionContext &fctx)
      : MemoryModel(fctx), True_(fctx.getZ3().bool_val(true)) {}

  z3::expr load(z3::expr, z3::expr, z3::expr) const override { return True_; }

  z3::expr store(z3::expr, z3::expr, z3::expr, z3::expr) const override {
    return True_;
  }

  virtual z3::expr copy(z3::expr, z3::expr) const override { return True_; }

  virtual z3::sort sort() const override {
    return True_.get_sort(); // doesn't really matter
  }
};

/**
 * Base class for memory models that allow addressable memory restriction.
 *
 * The models are parametrized by the number of address bits, `addr_bits`.
 * The analyzer then works under the assumption that only an `addr_bits`-subset
 * of the address space is used by the program, allowing for more efficient
 * representation of the memory state.
 */
class RestrictedSpace : public MemoryModel {
protected:
  const FunctionContext &FunctionContext_;
  z3::context &Z3_;
  z3::sort AddrSort_;

  /**
   * Constructs the memory model object.
   *
   * If the parameter `addr_bits` is non-positive, no address space
   * restriction is assumed. Otherwise, only `addr_bits` lowest bits
   * of every address are considered for all memory operations.
   */
  RestrictedSpace(const FunctionContext &fctx, int addr_bits = -1)
      : MemoryModel(fctx), FunctionContext_(fctx), Z3_(fctx.getZ3()),
        AddrSort_(
            Z3_.bv_sort(addr_bits > 0 ? addr_bits : fctx.getPointerSize())) {
    assert((int)AddrSort_.bv_size() <= fctx.getPointerSize());
  }

  virtual z3::expr loadRestricted(z3::expr, z3::expr mem,
                                  z3::expr addr) const = 0;
  virtual z3::expr storeRestricted(z3::expr pre, z3::expr post, z3::expr addr,
                                   z3::expr val) const = 0;

public:
  z3::expr load(z3::expr result, z3::expr mem,
                z3::expr addr) const override final;

  z3::expr store(z3::expr pre, z3::expr post, z3::expr addr,
                 z3::expr val) const override final;
};

/**
 * Models memory as an array of bytes with little-endian encoding.
 *
 * Allows for address space restriction for efficiency (see
 * RestrictedSpace).
 */
class LittleEndian : public RestrictedSpace {
private:
  z3::sort ValSort_;
  z3::sort Sort_;

protected:
  z3::expr loadRestricted(z3::expr, z3::expr mem, z3::expr addr) const override;

  z3::expr storeRestricted(z3::expr pre, z3::expr post, z3::expr addr,
                           z3::expr val) const override;

public:
  explicit LittleEndian(const FunctionContext &fctx, int addr_bits = -1)
      : RestrictedSpace(fctx, addr_bits), ValSort_(Z3_.bv_sort(8)),
        Sort_(Z3_.array_sort(AddrSort_, ValSort_)) {}

  z3::expr copy(z3::expr mem_pre, z3::expr mem_post) const override;
  z3::sort sort() const override { return Sort_; }
};

/**
 * Models memory as an array of multiple-byte-sized values.
 *
 * This model assumes that every memory access in the input program is aligned
 * to `align_bits` bits. If this is not the case, **the analysis result will be
 * unsound**.
 *
 * Memory is modeled as an array of values of `2**align_bits` bytes. All the
 * memory accesses in the input program must load or store values of this size
 * or the analysis will deliver imprecise results.
 */
class Aligned : public MemoryModel {
private:
  const FunctionContext &FunctionContext_;
  unsigned AlignmentBits_;
  z3::context &Z3_;
  z3::sort AddrSort_;
  z3::sort ValSort_;
  z3::sort Sort_;

public:
  Aligned(const FunctionContext &fctx, unsigned align_bits = 3)
      : MemoryModel(fctx), FunctionContext_(fctx), AlignmentBits_(align_bits),
        Z3_(fctx.getZ3()),
        AddrSort_(Z3_.bv_sort(fctx.getPointerSize() - align_bits)),
        ValSort_(Z3_.bv_sort(8 * (1 << align_bits))),
        Sort_(Z3_.array_sort(AddrSort_, ValSort_)) {
    assert(AlignmentBits_ > 0);
  }

  z3::expr load(z3::expr, z3::expr mem, z3::expr addr) const override;
  z3::expr store(z3::expr pre, z3::expr post, z3::expr addr,
                 z3::expr val) const override;
  z3::expr copy(z3::expr mem_pre, z3::expr mem_post) const override;
  z3::sort sort() const override { return Sort_; }
};
} // namespace memory
} // namespace symabs_ai
