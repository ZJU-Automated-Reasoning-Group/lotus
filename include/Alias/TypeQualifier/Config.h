/**
 * @file Config.h
 * @brief Configuration utilities for TypeQualifier analysis
 *
 * This file provides configuration setup functions for TypeQualifier analysis,
 * including definitions of known function categories such as heap allocation
 * functions, memory transfer functions, string functions, and more. These
 * configurations are used to identify special functions during analysis.
 *
 * @ingroup TypeQualifier
 */

#ifndef UBIANALYSIS_CONFIG_H
#define UBIANALYSIS_CONFIG_H
#include "Alias/TypeQualifier/IntGlobal.h"

#include <array>
#include <stack>
enum class FunctionModelKind {
  Unknown,
  Allocator,
  ZeroAllocator,
  Init,
  Copy,
  Transfer,
  ObjectSize,
  Ignore,
  Passthrough,
};

struct FunctionModel {
  FunctionModelKind kind = FunctionModelKind::Unknown;

  bool isAllocator() const {
    return kind == FunctionModelKind::Allocator ||
           kind == FunctionModelKind::ZeroAllocator;
  }

  bool zeroInitializes() const { return kind == FunctionModelKind::ZeroAllocator; }
};

class FunctionModelRegistry {
public:
  static FunctionModel lookup(llvm::StringRef name) {
    if (contains(kZeroAllocators(), name))
      return {FunctionModelKind::ZeroAllocator};
    if (contains(kAllocators(), name))
      return {FunctionModelKind::Allocator};
    if (contains(kTransferFns(), name))
      return {FunctionModelKind::Transfer};
    if (contains(kCopyFns(), name))
      return {FunctionModelKind::Copy};
    if (contains(kInitFns(), name))
      return {FunctionModelKind::Init};
    if (contains(kObjectSizeFns(), name))
      return {FunctionModelKind::ObjectSize};
    if (contains(kIgnoredFns(), name))
      return {FunctionModelKind::Ignore};
    if (name.startswith("printf"))
      return {FunctionModelKind::Passthrough};
    return {FunctionModelKind::Unknown};
  }

  template <typename Inserter>
  static void forEachName(FunctionModelKind kind, Inserter inserter) {
    switch (kind) {
    case FunctionModelKind::Allocator:
      for (const char *name : kAllocators())
        inserter(name);
      break;
    case FunctionModelKind::ZeroAllocator:
      for (const char *name : kZeroAllocators())
        inserter(name);
      break;
    case FunctionModelKind::Init:
      for (const char *name : kInitFns())
        inserter(name);
      break;
    case FunctionModelKind::Copy:
      for (const char *name : kCopyFns())
        inserter(name);
      break;
    case FunctionModelKind::Transfer:
      for (const char *name : kTransferFns())
        inserter(name);
      break;
    case FunctionModelKind::ObjectSize:
      for (const char *name : kObjectSizeFns())
        inserter(name);
      break;
    case FunctionModelKind::Ignore:
      for (const char *name : kIgnoredFns())
        inserter(name);
      break;
    case FunctionModelKind::Passthrough:
    case FunctionModelKind::Unknown:
      break;
    }
  }

private:
  template <size_t N>
  static bool contains(const std::array<const char *, N> &names,
                       llvm::StringRef name) {
    for (const char *candidate : names) {
      if (name == candidate)
        return true;
    }
    return false;
  }

  static const std::array<const char *, 9> &kAllocators() {
    static const std::array<const char *, 9> names = {{
        "__kmalloc", "__vmalloc", "vmalloc", "krealloc", "devm_kzalloc",
        "vzalloc", "malloc", "kmem_cache_alloc", "__alloc_skb",
    }};
    return names;
  }
  static const std::array<const char *, 1> &kZeroAllocators() {
    static const std::array<const char *, 1> names = {{
        "kzalloc",
    }};
    return names;
  }
  static const std::array<const char *, 4> &kTransferFns() {
    static const std::array<const char *, 4> names = {{
        "copy_to_user", "__copy_to_user", "copy_from_user", "__copy_from_user",
    }};
    return names;
  }
  static const std::array<const char *, 6> &kCopyFns() {
    static const std::array<const char *, 6> names = {{
        "memcpy", "llvm.memcpy.p0i8.p0i8.i32", "llvm.memcpy.p0i8.p0i8.i64",
        "memmove", "llvm.memmove.p0i8.p0i8.i32", "llvm.memmove.p0i8.p0i8.i64",
    }};
    return names;
  }
  static const std::array<const char *, 4> &kInitFns() {
    static const std::array<const char *, 4> names = {{
        "llvm.va_start", "memset", "llvm.memset.p0i8.i32",
        "llvm.memset.p0i8.i64",
    }};
    return names;
  }
  static const std::array<const char *, 1> &kObjectSizeFns() {
    static const std::array<const char *, 1> names = {{
        "llvm.objectsize.i64.p0i8",
    }};
    return names;
  }
  static const std::array<const char *, 4> &kIgnoredFns() {
    static const std::array<const char *, 4> names = {{
        "mcount", "llvm.dbg.declare", "llvm.dbg.value", "llvm.dbg.addr",
    }};
    return names;
  }
};

template <typename SetLike>
static void insertModelNames(SetLike &set, FunctionModelKind kind) {
  FunctionModelRegistry::forEachName(kind,
                                     [&](const char *name) { set.insert(name); });
}

static void initializeFunctionModelSets(GlobalContext &ctx) {
  if (ctx.functionModelsInitialized)
    return;
  insertModelNames(ctx.HeapAllocFuncs, FunctionModelKind::Allocator);
  insertModelNames(ctx.ZeroMallocFuncs, FunctionModelKind::ZeroAllocator);
  insertModelNames(ctx.TransferFuncs, FunctionModelKind::Transfer);
  insertModelNames(ctx.CopyFuncs, FunctionModelKind::Copy);
  insertModelNames(ctx.InitFuncs, FunctionModelKind::Init);
  insertModelNames(ctx.OtherFuncs, FunctionModelKind::Ignore);
  insertModelNames(ctx.ObjSizeFuncs, FunctionModelKind::ObjectSize);
  ctx.functionModelsInitialized = true;
}

static void SetStrFuncs(std::set<std::string> &StrFuncs) {
  std::string StrFN[] = {
      "strcmp", "strncmp", "strcpy",  "strlwr",  "strcat",
      "strlen", "strupr",  "strrchr", "strncat",
  };
  for (auto &F : StrFN) {
    StrFuncs.insert(F);
  }
}
// 10 candidates of the inderect caller which could be checked by casting
static void SetIndirectFuncs(std::set<std::string> &indirectFuncs) {
  std::string IndirectFN[] = {
      "kvm_vfio_group_is_coherent",
      //"kvm_vfio_external_group_match_file",
      //"kvm_device_ioctl_attr",
      //"kvm_device_ioctl",
      //"kvm_destroy_devices",
      //"sctp_inq_push",
      //"kvm_arch_vcpu_unblocking",
      //"kvm_vcpu_ioctl_set_cpuid2",
      //"hda_call_check_power_status",
      //"restore_mixer_state",
  };
  for (auto &F : IndirectFN) {
    indirectFuncs.insert(F);
  }
}
#ifdef FindAlloc
llvm::Value *FindAlloc(llvm::Value *V) {
  if (llvm::Instruction *I = dyn_cast<Instruction>(V)) {
    switch (I->getOpCode()) {
    case Instruction::Alloca:
    case Instruction::Add:
    case Instruction::FAdd:
    case Instruction::Sub:
    case Instruction::FSub:
    case Instruction::Mul:
    case Instruction::FMul:
    case Instruction::SDiv:
    case Instruction::UDiv:
    case Instruction::FDiv:
    case Instruction::SRem:
    case Instruction::URem:
    case Instruction::And:
    case Instruction::Or:
    case Instruction::Xor:
    case Instruction::LShr:
    case Instruction::AShr:
    case Instruction::Shl: {
      return I;
    }
    case Instruction::Load:
    case Instruction::SExt:
    case Instruction::ZExt:
    case Instruction::Trunc:
    case Instruction::IntToPtr:
    case Instruction::PtrToInt:
    case Instruction::Select: {
      return FindAlloc(I->getOperand(0));
    }
    case Instruction::GetElementPtr: {
      return FindAlloc(I->getOperand(0));
    }

    } // wsitch
  }
}
#endif

#endif // UBIANALYSIS_CONFIG_H
