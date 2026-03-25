#include "Verification/SymAbsAI/Core/MemoryModel.h"

#include "Verification/SymAbsAI/Utils/Config.h"
#include "Verification/SymAbsAI/Utils/Z3APIExtension.h"

namespace {
z3::expr extractByte(unsigned idx, z3::expr e) {
  return z3_ext::extract(8 * idx + 7, 8 * idx, e);
}

z3::expr makeDummyBVConst(z3::context &ctx, unsigned int bits) {
  static uint64_t id = 0;

  std::string name("__MM_Dummy_BV_Variable_");
  name = name + std::to_string(id++);

  return ctx.bv_const(name.c_str(), bits);
}

z3::expr makeDummyIntConst(z3::context &ctx) {
  static uint64_t id = 0;

  std::string name("__MM_Dummy_Int_Variable_");
  name = name + std::to_string(id++);

  return ctx.int_const(name.c_str());
}
} // namespace

namespace symabs_ai {
unique_ptr<MemoryModel> MemoryModel::New(const FunctionContext &fctx) {
  auto variant =
      fctx.getConfig().get<std::string>("MemoryModel", "Variant", "NoMemory");
  int addr_bits = fctx.getConfig().get<int>("MemoryModel", "AddressBits", -1);

  if (variant == "NoMemory") {
    return make_unique<memory::NoMemory>(fctx);
  } else if (variant == "LittleEndian") {
    return make_unique<memory::LittleEndian>(fctx, addr_bits);
  } else if (variant == "Aligned") {
    return make_unique<memory::Aligned>(fctx);
  } else if (variant == "BlockModel") {
    return make_unique<memory::BlockModel>(fctx);
  }

  llvm_unreachable("unknown memory model");
}

z3::sort MemoryModel::ptr_sort() const { return PtrSort_; }

z3::expr MemoryModel::allocate(z3::expr, z3::expr, z3::expr, z3::expr) const {
  return Fctx_.getZ3().bool_val(true);
}

z3::expr MemoryModel::deallocate(z3::expr, z3::expr, z3::expr) const {
  return Fctx_.getZ3().bool_val(true);
}

z3::expr MemoryModel::getelementptr(z3::expr result, z3::expr ptr,
                                    z3::expr offset) const {
  return result == ptr + offset;
}

z3::expr MemoryModel::ptrtoint(z3::expr result, z3::expr arg,
                               unsigned int size) const {
  return result == adjustBitwidth(arg, size);
}

z3::expr MemoryModel::inttoptr(z3::expr result, z3::expr arg) const {
  unsigned int size = Fctx_.getPointerSize();
  return result == adjustBitwidth(arg, size);
}

z3::expr MemoryModel::make_nullptr() const {
  return Fctx_.getZ3().bv_val(0, Fctx_.getPointerSize());
}

z3::expr MemoryModel::init_memory(z3::expr) const {
  return Fctx_.getZ3().bool_val(true);
}

namespace memory {
z3::expr RestrictedSpace::load(z3::expr result, z3::expr mem,
                               z3::expr addr) const {
  z3::expr small_addr = z3_ext::extract(AddrSort_.bv_size() - 1, 0, addr);
  return loadRestricted(result, mem, small_addr);
}

z3::expr RestrictedSpace::store(z3::expr pre, z3::expr post, z3::expr addr,
                                z3::expr val) const {
  z3::expr small_addr = z3_ext::extract(AddrSort_.bv_size() - 1, 0, addr);
  return storeRestricted(pre, post, small_addr, val);
}

z3::expr LittleEndian::loadRestricted(z3::expr result, z3::expr mem,
                                      z3::expr addr) const {
  unsigned bitwidth = result.get_sort().bv_size();
  assert(bitwidth % 8 == 0);
  unique_ptr<z3::expr> res_val;

  for (unsigned byte = 0; byte < bitwidth / 8; byte++) {
    z3::expr new_byte =
        select(mem, addr + Z3_.bv_val(byte, AddrSort_.bv_size()));

    if (res_val)
      res_val.reset(new z3::expr(z3_ext::concat(new_byte, *res_val)));
    else
      res_val.reset(new z3::expr(new_byte));
  }

  return result == *res_val;
}

z3::expr LittleEndian::storeRestricted(z3::expr pre, z3::expr post,
                                       z3::expr addr, z3::expr val) const {
  z3::expr formula = Z3_.bool_val(true);
  unsigned bitwidth = val.get_sort().bv_size();
  assert(bitwidth % 8 == 0);

  for (unsigned byte = 0; byte < bitwidth / 8; byte++) {
    z3::expr sub_val = extractByte(byte, val);
    z3::expr sub_addr = addr + Z3_.bv_val(byte, AddrSort_.bv_size());
    z3::expr post_expr = z3::store(pre, sub_addr, sub_val);
    formula = formula && (post == post_expr);
  }

  return formula;
}

z3::expr LittleEndian::copy(z3::expr mem_pre, z3::expr mem_post) const {
  return mem_pre == mem_post;
}

z3::expr Aligned::load(z3::expr result, z3::expr mem, z3::expr addr) const {
  if (result.get_sort().bv_size() != ValSort_.bv_size()) {
    // load of a different size; assume it can result in any value (but
    // still constrain the address to be aligned)
    z3::expr zero_bits = z3_ext::extract(AlignmentBits_ - 1, 0, addr);
    return zero_bits == Z3_.bv_val(0, AlignmentBits_);
  }

  z3::expr idx =
      z3_ext::extract(addr.get_sort().bv_size() - 1, AlignmentBits_, addr);
  z3::expr zero_bits = z3_ext::extract(AlignmentBits_ - 1, 0, addr);

  return result == z3::select(mem, idx) &&
         zero_bits == Z3_.bv_val(0, AlignmentBits_);
}

z3::expr Aligned::store(z3::expr pre, z3::expr post, z3::expr addr,
                        z3::expr val) const {
  if (val.get_sort().bv_size() != ValSort_.bv_size()) {
    // assume anything can happen
    // TODO: in some cases the rest of the memory can be preserved
    z3::expr zero_bits = z3_ext::extract(AlignmentBits_ - 1, 0, addr);
    return zero_bits == Z3_.bv_val(0, AlignmentBits_);
  }

  z3::expr idx =
      z3_ext::extract(addr.get_sort().bv_size() - 1, AlignmentBits_, addr);
  z3::expr zero_bits = z3_ext::extract(AlignmentBits_ - 1, 0, addr);

  return post == z3::store(pre, idx, val) &&
         zero_bits == Z3_.bv_val(0, AlignmentBits_);
}

z3::expr Aligned::copy(z3::expr mem_pre, z3::expr mem_post) const {
  return mem_pre == mem_post;
}

BlockModel::BlockModel(const FunctionContext &fctx)
    : MemoryModel(fctx), Sort_(fctx.getZ3()), PtrSort_(fctx.getZ3()),
      BlockIdSort_(fctx.getZ3().int_sort()), MkMem_(fctx.getZ3()),
      GetNextAlloc_(fctx.getZ3()), GetMap_(fctx.getZ3()), MkPtr_(fctx.getZ3()),
      GetIdx_(fctx.getZ3()), GetBlock_(fctx.getZ3()) {
  auto &ctx = Fctx_.getZ3();
  unsigned bits = Fctx_.getPointerSize();

  makePairSort(ctx, PtrSort_, GetIdx_, GetBlock_, MkPtr_, "get_idx",
               Fctx_.getZ3().bv_sort(bits), "get_block", BlockIdSort_, "ptr");

  makePairSort(ctx, Sort_, GetNextAlloc_, GetMap_, MkMem_, "get_next_alloc",
               BlockIdSort_, "get_map",
               ctx.array_sort(BlockIdSort_, ctx.bv_sort(bits)), "mem");
}

z3::expr BlockModel::copy(z3::expr mem_before, z3::expr mem_after) const {
  return mem_before == mem_after;
}

z3::sort BlockModel::sort() const { return Sort_; }

z3::sort BlockModel::ptr_sort() const { return PtrSort_; }

z3::expr BlockModel::allocate(z3::expr mem_before, z3::expr mem_after,
                              z3::expr result, z3::expr size) const {
  unsigned int bits = Fctx_.getPointerSize();
  assert(size.is_bv() && size.get_sort().bv_size() == bits);
  z3::expr idx = Fctx_.getZ3().bv_val(0, bits);

  z3::expr next_alloc = GetNextAlloc_(mem_before);
  z3::expr next_alloc_new = next_alloc + 1;
  z3::expr map_new = z3::store(GetMap_(mem_before), next_alloc, size);
  z3::expr allocate_block = mem_after == MkMem_(next_alloc_new, map_new);

  return (result == MkPtr_(idx, next_alloc)) && allocate_block;
}

z3::expr BlockModel::deallocate(z3::expr mem_before, z3::expr mem_after,
                                z3::expr ptr) const {
  unsigned int bits = Fctx_.getPointerSize();
  z3::expr block = GetBlock_(ptr);
  z3::expr zero = Fctx_.getZ3().bv_val(0, bits);

  z3::expr next_alloc = GetNextAlloc_(mem_before);
  z3::expr map_new = z3::store(GetMap_(mem_before), block, zero);

  return mem_after == MkMem_(next_alloc, map_new);
}

z3::expr BlockModel::getelementptr(z3::expr result, z3::expr ptr,
                                   z3::expr offset) const {
  z3::expr val = GetIdx_(ptr) + offset;
  return result == MkPtr_(val, GetBlock_(ptr));
}

z3::expr BlockModel::ptrtoint(z3::expr result, z3::expr arg,
                              unsigned int size) const {
  // We know nothing about actual pointer values.
  return Fctx_.getZ3().bool_val(true);
}

z3::expr BlockModel::inttoptr(z3::expr result, z3::expr arg) const {
  int bits = Fctx_.getPointerSize();
  z3::expr idx = makeDummyBVConst(Fctx_.getZ3(), bits);
  z3::expr block = makeDummyIntConst(Fctx_.getZ3());
  return result == MkPtr_(idx, block);
}

z3::expr BlockModel::make_nullptr() const {
  z3::expr idx = Fctx_.getZ3().bv_val(0, Fctx_.getPointerSize());
  z3::expr block = Fctx_.getZ3().int_val(0);
  return MkPtr_(idx, block);
}

z3::expr BlockModel::get_ptr_value(z3::expr) const {
  // This might cause non-termination issues, but shouldn't be used anyway.
  // (However, it is used in the ConcreteState constructor.)
  // TODO maybe return pointer index instead.
  return makeDummyBVConst(Fctx_.getZ3(), Fctx_.getPointerSize());
}

z3::expr BlockModel::no_alias(z3::expr p, z3::expr q) const {
  return GetBlock_(p) != GetBlock_(q);
}

z3::expr BlockModel::region_size(z3::expr mem, z3::expr p) const {
  z3::expr idx = GetIdx_(p);
  z3::expr block = GetBlock_(p);
  z3::expr bound = select(GetMap_(mem), block);
  return bound - idx;
}

z3::expr BlockModel::valid_region(z3::expr mem, z3::expr p) const {
  z3::expr idx = GetIdx_(p);
  z3::expr block = GetBlock_(p);
  z3::expr bound = select(GetMap_(mem), block);

  return z3::ult(idx, bound) && (block < GetNextAlloc_(mem));
}

z3::expr BlockModel::init_memory(z3::expr mm) const {
  z3::expr int_zero = Fctx_.getZ3().int_val(0);
  z3::expr bv_zero = Fctx_.getZ3().bv_val(0, Fctx_.getPointerSize());

  // use only positive indices in the Map for new allocations
  z3::expr pos_alloc = GetNextAlloc_(mm) > int_zero;

  // define the 0-th entry of the Map to be a null region
  z3::expr null_block = z3::select(GetMap_(mm), int_zero) == bv_zero;

  return null_block && pos_alloc;
}

} // namespace memory
} // namespace symabs_ai
