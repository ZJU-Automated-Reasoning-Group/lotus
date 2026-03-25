// Implementation of GlobalPointerAnalysis.
//
// This class is responsible for the initial setup of the pointer analysis state
// derived from global variables and their initializers in the LLVM module.
//
// Key responsibilities:
// 1. Register all global variables and functions with the PointerManager.
// 2. Allocate MemoryObjects for global variables and functions.
// 3. Process initializers (scalar, array, struct) to populate the initial
//    points-to graph (Environment and Store).
// 4. Handle constant expressions (GEP, BitCast) in initializers.

#include "Alias/TPA/PointerAnalysis/Analysis/GlobalPointerAnalysis.h"

#include "Alias/TPA/Context/Context.h"
#include "Alias/TPA/PointerAnalysis/FrontEnd/Type/TypeMap.h"
#include "Alias/TPA/PointerAnalysis/MemoryModel/MemoryManager.h"
#include "Alias/TPA/PointerAnalysis/MemoryModel/PointerManager.h"
#include "Alias/TPA/Util/Log.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace tpa {

// Helper to check if a type is a scalar non-pointer type (e.g., int, float).
// These types are generally uninteresting for pointer analysis unless cast to
// pointers.
static bool isScalarNonPointerType(const Type *type) {
  assert(type != nullptr);
  return type->isSingleValueType() && !type->isPointerTy();
}

// Calculates the byte offset for a sequence of GEP indices.
//
// Fix #1: Removed use of the deprecated getPointerElementType() API (removed
// in LLVM 17 with opaque pointers). The pointer-arithmetic branch now requires
// the caller to pass the pointee type explicitly via `baseType`.
//
// Fix #2: The caller (processConstantGEP) previously passed
// `baseVal->getType()` (a pointer type, e.g. `i32*`) instead of the pointee
// type (`i32`). The function now takes the *pointee* type directly, so the
// first index correctly indexes into the pointed-to aggregate rather than
// hitting the pointer-arithmetic fallback.
//
// Parameters:
//   dataLayout  - module data layout
//   pointeeType - the type that the base pointer points TO (not the pointer
//                 type itself)
//   indexes     - GEP index operands (excluding the base pointer operand)
static unsigned calculateIndexedOffset(const DataLayout &dataLayout,
                                       Type *pointeeType,
                                       ArrayRef<Value *> indexes) {
  unsigned offset = 0;
  Type *currentType = pointeeType;

  for (const auto *idxIt = indexes.begin(); idxIt != indexes.end(); ++idxIt) {
    Value *idxVal = *idxIt;

    if (auto *ci = dyn_cast<ConstantInt>(idxVal)) {
      uint64_t fieldIdx = ci->getZExtValue();

      if (auto *structTy = dyn_cast<StructType>(currentType)) {
        // Handle struct member access
        auto *structLayout = dataLayout.getStructLayout(structTy);
        offset += structLayout->getElementOffset(fieldIdx);
        currentType = structTy->getElementType(fieldIdx);
      } else if (auto *arrayTy = dyn_cast<ArrayType>(currentType)) {
        // Handle array element access
        Type *elemType = arrayTy->getElementType();
        uint64_t elemSize = dataLayout.getTypeAllocSize(elemType);
        offset += fieldIdx * elemSize;
        currentType = elemType;
      } else if (auto *vecTy = dyn_cast<VectorType>(currentType)) {
        // Handle vector element access
        Type *elemType = vecTy->getElementType();
        uint64_t elemSize = dataLayout.getTypeAllocSize(elemType);
        offset += fieldIdx * elemSize;
        currentType = elemType;
      } else {
        // Pointer arithmetic: the current type is a pointer; the index steps
        // over elements of the pointee type.
        // Fix #1: We no longer call getPointerElementType() here. Instead we
        // rely on the fact that for a well-formed constant GEP the first index
        // always steps over the base pointee type, which is passed in as
        // `pointeeType` and is already set as `currentType` on the first
        // iteration. Subsequent iterations should never reach this branch for
        // a valid GEP (they will always be struct/array/vector). If we do
        // reach here for a later index it means the IR is unusual; treat the
        // offset as unknown (0) and stop.
        LOG_WARN("calculateIndexedOffset: unexpected pointer type at index {}; "
                 "treating remaining offset as 0",
                 static_cast<unsigned>(idxIt - indexes.begin()));
        break;
      }
    } else {
      // Non-constant index in a constant GEP — should not happen for a
      // ConstantExpr GEP, but handle gracefully instead of crashing.
      LOG_WARN("calculateIndexedOffset: non-constant index in constant GEP; "
               "treating remaining offset as 0");
      break;
    }
  }

  return offset;
}

GlobalPointerAnalysis::GlobalPointerAnalysis(PointerManager &p,
                                             MemoryManager &m, const TypeMap &t)
    : ptrManager(p), memManager(m), typeMap(t),
      globalCtx(context::Context::getGlobalContext()) {}

// Creates Pointer and MemoryObject representations for all global variables.
// Updates the Environment (env) to map the global pointer to its memory object.
void GlobalPointerAnalysis::createGlobalVariables(const Module &module,
                                                  Env &env) {
  for (auto const &gVar : module.globals()) {
    // Create pointer first (represents the address of the global)
    const auto *gPtr = ptrManager.getOrCreatePointer(globalCtx, &gVar);

    // Create memory object (represents the storage of the global)
    auto *gType = gVar.getType()->getNonOpaquePointerElementType();
    const auto *typeLayout = typeMap.lookup(gType);
    assert(typeLayout != nullptr);
    const auto *gObj = memManager.allocateGlobalMemory(&gVar, typeLayout);

    // Now add the top-level mapping: gPtr -> { gObj }
    env.insert(gPtr, gObj);
  }
}

// Creates Pointer and MemoryObject representations for all functions.
// This is necessary because functions can be taken as addresses (function
// pointers).
void GlobalPointerAnalysis::createFunctions(const llvm::Module &module,
                                            Env &env) {
  for (auto const &f : module) {
    // For each function, regardless of whether it is internal or external, and
    // regardless of whether it has its address taken or not, we are going to
    // create a function pointer and a function object for it.
    const auto *fPtr = ptrManager.getOrCreatePointer(globalCtx, &f);
    const auto *fObj = memManager.allocateMemoryForFunction(&f);

    // Add the top-level mapping: fPtr -> { fObj }
    env.insert(fPtr, fObj);
  }
}

// Retrieves the memory object associated with a global value from the
// environment.
const MemoryObject *
GlobalPointerAnalysis::getGlobalObject(const GlobalValue *gv, const Env &env) {
  const auto *iPtr = ptrManager.getPointer(globalCtx, gv);
  assert(iPtr != nullptr && "gv ptr not found");
  const auto iSet = env.lookup(iPtr);
  assert(iSet.size() == 1 && "Cannot find pSet of gv ptr!");
  const auto *obj = *iSet.begin();
  assert(obj != nullptr);
  return obj;
}

// Iterates over all globals to process their initializers.
// Populates the Store with the values written by initializers.
void GlobalPointerAnalysis::initializeGlobalValues(const llvm::Module &module,
                                                   EnvStore &envStore) {
  DataLayout dataLayout(&module);
  for (auto const &gVar : module.globals()) {
    const auto *gObj = getGlobalObject(&gVar, envStore.first);

    if (gVar.hasInitializer()) {
      processGlobalInitializer(gObj, gVar.getInitializer(), envStore,
                               dataLayout);
    } else {
      // If gVar doesn't have an initializer, since we are assuming a
      // whole-program analysis, the value must be external (e.g. struct FILE*
      // stdin). To be conservative, assume that those "external" globals can
      // point to anything (UniversalObject).
      envStore.second.strongUpdate(
          gObj, PtsSet::getSingletonSet(MemoryManager::getUniversalObject()));
    }
  }
}

// Analyzes a ConstantExpr GEP to determine the base global variable and total
// offset. Handles nested BitCasts and recursive GEPs.
std::pair<const llvm::GlobalVariable *, size_t>
GlobalPointerAnalysis::processConstantGEP(const llvm::ConstantExpr *cexpr,
                                          const DataLayout &dataLayout) {
  assert(cexpr->getOpcode() == llvm::Instruction::GetElementPtr);

  auto *baseVal = cexpr->getOperand(0);
  auto indexes = llvm::SmallVector<llvm::Value *, 4>(cexpr->op_begin() + 1,
                                                     cexpr->op_end());

  // Fix #2: Pass the *pointee* type (source element type of the GEP), not the
  // pointer type of the base operand. Previously `baseVal->getType()` was
  // passed, which is a pointer type (e.g. `i32*`). calculateIndexedOffset
  // expects the type being indexed into (e.g. `i32` or a struct type).
  //
  // getSourceElementType() is defined on GEPOperator (llvm/IR/Operator.h),
  // which ConstantExpr with a GEP opcode inherits from.  We must cast to
  // GEPOperator to access it — ConstantExpr itself does not expose the method
  // directly in LLVM 14.
  const auto *gepOp = cast<GEPOperator>(cexpr);
  Type *sourceElemType = gepOp->getSourceElementType();
  unsigned offset = calculateIndexedOffset(dataLayout, sourceElemType, indexes);

  // The loop is written for bitcast handling and nested constant expressions
  while (true) {
    if (auto *gVar = dyn_cast<GlobalVariable>(baseVal)) {
      return std::make_pair(gVar, offset);
    } else if (auto *ga = dyn_cast<GlobalAlias>(baseVal)) {
      // GlobalAlias is a valid LLVM construct (e.g., @foo = alias i32*,
      // i32* @bar). Resolve the alias to its aliasee and continue the loop.
      // If the aliasee is not a GlobalVariable (e.g., it is itself an alias
      // chain or a function), we conservatively return nullptr (unknown base)
      // to avoid infinite loops.
      auto *aliasee = const_cast<Constant *>(ga->getAliasee());
      if (aliasee == nullptr)
        return std::make_pair(nullptr, 0);
      baseVal = aliasee;
      // Continue loop to resolve the aliasee.
    } else if (auto *ce = dyn_cast<ConstantExpr>(baseVal)) {
      switch (ce->getOpcode()) {
      case Instruction::GetElementPtr: {
        // Accumulate offset from nested GEP
        const auto baseOffsetPair = processConstantGEP(ce, dataLayout);
        return std::make_pair(baseOffsetPair.first,
                              baseOffsetPair.second + offset);
      }
      case Instruction::IntToPtr:
        // IntToPtr results in unknown base
        return std::make_pair(nullptr, 0);
      case Instruction::BitCast:
        // Strip bitcast and continue
        baseVal = ce->getOperand(0);
        // Don't return. Keep looping on baseVal
        break;
      default: {
        std::string ceStr;
        raw_string_ostream ceOS(ceStr);
        ceOS << *ce;
        ceOS.flush();
        LOG_ERROR("Constant expr not yet handled in global initializer: {}",
                  ceStr);
        llvm_unreachable("Unknown constantexpr!");
      }
      }
    } else {
      // Unknown base (e.g., a function, BlockAddress, or other constant).
      // Conservatively treat as unknown rather than crashing.
      std::string baseValStr;
      raw_string_ostream baseValOS(baseValStr);
      baseValOS << *baseVal;
      baseValOS.flush();
      LOG_WARN("Unknown constant GEP base (treating as universal): {}",
               baseValStr);
      return std::make_pair(nullptr, 0);
    }
  }
}

// Handles scalar initializers (pointer values).
// Updates the store at gObj to point to the target of the initializer.
void GlobalPointerAnalysis::processGlobalScalarInitializer(
    const MemoryObject *gObj, const llvm::Constant *initializer,
    EnvStore &envStore, const DataLayout &dataLayout) {
  if (!initializer->getType()->isPointerTy())
    return;

  if (initializer->isNullValue())
    envStore.second.insert(gObj, memManager.getNullObject());
  else if (isa<UndefValue>(initializer))
    envStore.second.strongUpdate(
        gObj, PtsSet::getSingletonSet(MemoryManager::getUniversalObject()));
  else if (isa<GlobalVariable>(initializer) || isa<Function>(initializer)) {
    const auto *gv = cast<GlobalValue>(initializer);
    const auto *tgtObj = getGlobalObject(gv, envStore.first);
    envStore.second.insert(gObj, tgtObj);
  } else if (const auto *ce = dyn_cast<ConstantExpr>(initializer)) {
    switch (ce->getOpcode()) {
    case Instruction::GetElementPtr: {
      // Offset calculation for GEP constant expressions
      const auto baseOffsetPair = processConstantGEP(ce, dataLayout);
      if (baseOffsetPair.first == nullptr)
        envStore.second.strongUpdate(
            gObj, PtsSet::getSingletonSet(MemoryManager::getUniversalObject()));
      else {
        const auto *tgtObj =
            getGlobalObject(baseOffsetPair.first, envStore.first);
        const auto *offsetObj =
            memManager.offsetMemory(tgtObj, baseOffsetPair.second);
        envStore.second.insert(gObj, offsetObj);
      }
      break;
    }
    case Instruction::IntToPtr: {
      // By default, clang won't generate global pointer arithmetic as
      // ptrtoint+inttoptr, so we will do the simplest thing here: treat as
      // universal
      envStore.second.insert(gObj, MemoryManager::getUniversalObject());
      break;
    }
    case Instruction::BitCast: {
      // Recursively process the operand of the bitcast
      processGlobalInitializer(gObj, ce->getOperand(0), envStore, dataLayout);
      break;
    }
    default:
      break;
    }
  } else {
    std::string initStr;
    raw_string_ostream initOS(initStr);
    initOS << *initializer;
    initOS.flush();
    LOG_ERROR("Unsupported constant pointer: {}", initStr);
    llvm_unreachable("Unsupported constant pointer!");
  }
}

// Handles struct initializers by iterating over fields and offsets.
void GlobalPointerAnalysis::processGlobalStructInitializer(
    const MemoryObject *gObj, const llvm::Constant *initializer,
    EnvStore &envStore, const DataLayout &dataLayout) {
  auto *stType = cast<StructType>(initializer->getType());

  // Structs are treated field-sensitively
  const auto *stLayout = dataLayout.getStructLayout(stType);
  for (unsigned i = 0, e = initializer->getNumOperands(); i != e; ++i) {
    unsigned offset = stLayout->getElementOffset(i);

    const Constant *subInitializer = nullptr;
    if (const auto *caz = dyn_cast<ConstantAggregateZero>(initializer))
      subInitializer = caz->getStructElement(i);
    else if (const auto *undef = dyn_cast<UndefValue>(initializer))
      subInitializer = undef->getStructElement(i);
    else
      subInitializer = cast<Constant>(initializer->getOperand(i));

    auto *subInitType = subInitializer->getType();
    if (isScalarNonPointerType(subInitType))
      // Not an interesting field. Skip it.
      continue;

    // Apply the offset to get the memory object for the field
    const auto *offsetObj = memManager.offsetMemory(gObj, offset);
    // Recursively process the field initializer
    processGlobalInitializer(offsetObj, subInitializer, envStore, dataLayout);
  }
}

// Handles array initializers.
// Note: Arrays are often "collapsed" in pointer analysis (field-insensitive for
// array elements), but this implementation iterates all elements. If
// offsetMemory collapses them, that logic is in MemoryManager.
void GlobalPointerAnalysis::processGlobalArrayInitializer(
    const MemoryObject *gObj, const llvm::Constant *initializer,
    EnvStore &envStore, const DataLayout &dataLayout) {
  auto *arrayType = cast<ArrayType>(initializer->getType());
  auto *elemType = arrayType->getElementType();

  if (!isScalarNonPointerType(elemType)) {
    // Arrays/vectors are collapsed into a single element
    for (unsigned i = 0, e = initializer->getNumOperands(); i < e; ++i) {
      const Constant *elem = nullptr;
      if (const auto *caz = dyn_cast<ConstantAggregateZero>(initializer))
        elem = caz->getSequentialElement();
      else if (const auto *undef = dyn_cast<UndefValue>(initializer))
        elem = undef->getSequentialElement();
      else
        elem = cast<Constant>(initializer->getOperand(i));

      processGlobalInitializer(gObj, elem, envStore, dataLayout);
    }
  }
}

// Dispatch method for different initializer types.
void GlobalPointerAnalysis::processGlobalInitializer(
    const MemoryObject *gObj, const Constant *initializer, EnvStore &envStore,
    const DataLayout &dataLayout) {
  if (initializer->getType()->isSingleValueType())
    processGlobalScalarInitializer(gObj, initializer, envStore, dataLayout);
  else if (initializer->getType()->isStructTy())
    processGlobalStructInitializer(gObj, initializer, envStore, dataLayout);
  else if (initializer->getType()->isArrayTy())
    processGlobalArrayInitializer(gObj, initializer, envStore, dataLayout);
  else {
    std::string initStr;
    raw_string_ostream initOS(initStr);
    initOS << *initializer;
    initOS.flush();
    LOG_ERROR("Unknown initializer type: {}", initStr);
    llvm_unreachable("Unknown initializer type");
  }
}

// Initializes special pointer objects (Universal, Null).
void GlobalPointerAnalysis::initializeSpecialPointerObject(const Module &module,
                                                           EnvStore &envStore) {
  const auto *uPtr = ptrManager.setUniversalPointer(
      UndefValue::get(Type::getInt8PtrTy(module.getContext())));
  const auto *uLoc = MemoryManager::getUniversalObject();
  envStore.first.insert(uPtr, uLoc);
  envStore.second.insert(uLoc, uLoc);
  const auto *nPtr = ptrManager.setNullPointer(
      ConstantPointerNull::get(Type::getInt8PtrTy(module.getContext())));
  const auto *nLoc = memManager.getNullObject();
  envStore.first.insert(nPtr, nLoc);
}

// Main driver for the global pointer analysis phase.
// Returns a pair of (Env, Store) representing the initial state.
std::pair<Env, Store> GlobalPointerAnalysis::runOnModule(const Module &module) {
  EnvStore envStore;

  // Set up the points-to relations of uPtr, uObj and nullPtr
  LOG_DEBUG("Initializing special pointer objects (universal, null)");
  initializeSpecialPointerObject(module, envStore);

  // First, scan through all the global values and register them in ptrManager.
  // This scan should precede variable initialization because the initialization
  // may refer to another global value defined "below" it.
  unsigned numGlobals = module.getGlobalList().size();
  unsigned numFunctions = module.getFunctionList().size();
  LOG_INFO("  Creating {} global variables and {} function pointers...",
           numGlobals, numFunctions);
  createGlobalVariables(module, envStore.first);
  createFunctions(module, envStore.first);

  // After all the global values are defined, go ahead and process the
  // initializers to populate the store.
  LOG_INFO("  Processing global initializers...");
  initializeGlobalValues(module, envStore);
  LOG_INFO("  Global initialization completed");

  // Return the constructed environment and store.
  return envStore;
}

} // namespace tpa
