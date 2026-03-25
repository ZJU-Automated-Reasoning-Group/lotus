// Implementation of the PointerManager.
//
// The PointerManager is responsible for managing the `Pointer` abstraction.
// A `Pointer` in TPA represents an SSA value (register) in a specific context.
//
// Key Responsibilities:
// 1. Value Canonicalization: Handling LLVM value quirks (stripping bitcasts,
//    handling trivial PHIs) to map equivalent values to the same Pointer.
// 2. Pointer Interning: Maintaining a unique set of Pointer objects to allow
//    fast pointer equality comparison.
// 3. Mapping: Maintaining the relationship between LLVM Values and TPA
// Pointers.
// 4. Special Pointers: Managing the Universal and Null pointers.

#include "Alias/TPA/PointerAnalysis/MemoryModel/PointerManager.h"

#include "Alias/TPA/Context/Context.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Instructions.h>

using namespace context;

namespace tpa {

// Helper to strip non-functional wrappers from an LLVM Value.
// - Strips pointer casts (BitCast, GEP with 0 offset, etc.)
// - Collapses trivial PHI nodes (single operand).
// - Normalizes IntToPtr to Undef (conservative).
const llvm::Value *canonicalizeValue(const llvm::Value *value) {
  assert(value != nullptr);
  value = value->stripPointerCasts();

  if (const auto *phiNode = llvm::dyn_cast<llvm::PHINode>(value)) {
    const llvm::Value *rhs = nullptr;
    for (auto &op : phiNode->operands()) {
      auto *val = op.get()->stripPointerCasts();
      if (rhs == nullptr)
        rhs = val;
      else if (val != rhs) {
        // PHI has different incoming values, cannot simplify.
        rhs = nullptr;
        break;
      }
    }

    if (rhs != nullptr)
      value = rhs;
  } else if (llvm::isa<llvm::IntToPtrInst>(value))
    value = llvm::UndefValue::get(value->getType());

  return value;
}

PointerManager::PointerManager() : uPtr(nullptr), nPtr(nullptr) {}

// Creates or retrieves a Pointer object.
// Interns the pointer in `ptrSet`.
const Pointer *PointerManager::buildPointer(const context::Context *ctx,
                                            const llvm::Value *val) {
  auto ptr = Pointer(ctx, val);
  auto itr = ptrSet.find(ptr);
  if (itr != ptrSet.end())
    return &*itr;

  itr = ptrSet.insert(itr, ptr);
  const auto *ret = &*itr;
  // Record reverse mapping: Value -> List of Pointers (one per context)
  valuePtrMap[val].push_back(ret);
  return ret;
}

// Initializes the universal pointer (represents unknown/all pointers).
const Pointer *PointerManager::setUniversalPointer(const llvm::UndefValue *v) {
  assert(uPtr == nullptr);
  assert(v->getType() == llvm::Type::getInt8PtrTy(v->getContext()));
  uPtr = buildPointer(Context::getGlobalContext(), v);
  return uPtr;
}

const Pointer *PointerManager::getUniversalPointer() const {
  assert(uPtr != nullptr);
  return uPtr;
}

// Initializes the null pointer.
const Pointer *
PointerManager::setNullPointer(const llvm::ConstantPointerNull *v) {
  assert(nPtr == nullptr);
  assert(v->getType() == llvm::Type::getInt8PtrTy(v->getContext()));
  nPtr = buildPointer(Context::getGlobalContext(), v);
  return nPtr;
}

const Pointer *PointerManager::getNullPointer() const {
  assert(nPtr != nullptr);
  return nPtr;
}

// Retrieves an existing pointer. Returns nullptr if not found.
// Handles special cases (Null, Undef/Universal, Globals).
const Pointer *PointerManager::getPointer(const Context *ctx,
                                          const llvm::Value *val) const {
  assert(ctx != nullptr && val != nullptr);

  val = canonicalizeValue(val);

  if (llvm::isa<llvm::ConstantPointerNull>(val))
    return nPtr;
  else if (llvm::isa<llvm::UndefValue>(val))
    return uPtr;
  else if (llvm::isa<llvm::GlobalValue>(val))
    // Globals always live in the global context
    ctx = Context::getGlobalContext();

  auto itr = ptrSet.find(Pointer(ctx, val));
  if (itr == ptrSet.end())
    return nullptr;
  else
    return &*itr;
}

// Retrieves a pointer, creating it if it doesn't exist.
const Pointer *PointerManager::getOrCreatePointer(const Context *ctx,
                                                  const llvm::Value *val) {
  assert(ctx != nullptr && val != nullptr);

  val = canonicalizeValue(val);

  if (llvm::isa<llvm::ConstantPointerNull>(val))
    return nPtr;
  else if (llvm::isa<llvm::UndefValue>(val))
    return uPtr;
  else if (llvm::isa<llvm::GlobalValue>(val))
    ctx = Context::getGlobalContext();

  return buildPointer(ctx, val);
}

// Finds all Pointers associated with a given LLVM Value across all contexts.
PointerManager::PointerVector
PointerManager::getPointersWithValue(const llvm::Value *val) const {
  PointerVector vec;

  val = canonicalizeValue(val);

  if (llvm::isa<llvm::ConstantPointerNull>(val))
    vec.push_back(nPtr);
  else if (llvm::isa<llvm::UndefValue>(val))
    vec.push_back(uPtr);
  else {
    auto itr = valuePtrMap.find(val);
    if (itr != valuePtrMap.end())
      vec = itr->second;
  }

  return vec;
}

} // namespace tpa