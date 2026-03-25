// Implementation of InstructionTranslator.
//
// Translates individual LLVM Instructions into TPA CFGNodes.
//
// Key Responsibilities:
// 1. Identify relevant instructions (Alloca, Load, Store, Call, GEP, etc.).
// 2. Filter irrelevant instructions (arithmetic, etc.).
// 3. Handle complicated logic like PHI nodes and Selects (translating them to
// Copy).
// 4. Handle "extraction" patterns (ExtractValue, ExtractElement) to recover
// pointers.

#include "Alias/TPA/PointerAnalysis/FrontEnd/CFG/InstructionTranslator.h"

#include "Alias/TPA/Context/Context.h"
#include "Alias/TPA/PointerAnalysis/FrontEnd/Type/TypeMap.h"
#include "Alias/TPA/PointerAnalysis/Program/CFG/CFG.h"
#include "Alias/TPA/Util/Log.h"

#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace tpa {

// Helper to create a Copy node for PHI/Select instructions.
tpa::CFGNode *InstructionTranslator::createCopyNode(
    const Instruction *inst, const SmallPtrSetImpl<const Value *> &srcs) {
  assert(inst != nullptr && srcs.size() > 0u);
  auto srcVals = std::vector<const Value *>(srcs.begin(), srcs.end());
  return cfg.create<tpa::CopyCFGNode>(inst, std::move(srcVals));
}

tpa::CFGNode *InstructionTranslator::visitAllocaInst(AllocaInst &allocaInst) {
  // Only interested in allocas of pointer types (pointer to pointer),
  // OR allocas of anything if we track address-taken?
  // Actually, TPA tracks allocations of memory blocks.
  // Wait, `allocaInst.getType()` is `T*`. `allocaInst.getAllocatedType()` is
  // `T`. The check below says `isPointerTy()`. This implies we only track stack
  // allocations if the *result* is treated as a pointer (always true)
  // or maybe it means we only care if we allocate a pointer on the stack?
  //
  // Re-reading code: `visitAllocaInst` assumes `getType()->isPointerTy()`,
  // which is always true.
  assert(allocaInst.getType()->isPointerTy());

  const auto *allocType = typeMap.lookup(allocaInst.getAllocatedType());
  assert(allocType != nullptr && "Alloc type not found");

  return cfg.create<tpa::AllocCFGNode>(&allocaInst, allocType);
}

tpa::CFGNode *InstructionTranslator::visitLoadInst(LoadInst &loadInst) {
  // We only care about loading POINTERS. Data flow of non-pointers is
  // irrelevant.
  if (!loadInst.getType()->isPointerTy())
    return nullptr;

  auto *dstVal = &loadInst;
  auto *srcVal = loadInst.getPointerOperand()->stripPointerCasts();
  return cfg.create<tpa::LoadCFGNode>(dstVal, srcVal);
}

tpa::CFGNode *InstructionTranslator::visitStoreInst(StoreInst &storeInst) {
  auto *valOp = storeInst.getValueOperand();
  // We only care about storing POINTERS into memory.
  if (!valOp->getType()->isPointerTy())
    return nullptr;
  auto *ptrOp = storeInst.getPointerOperand();

  return cfg.create<tpa::StoreCFGNode>(ptrOp->stripPointerCasts(),
                                       valOp->stripPointerCasts());
}

tpa::CFGNode *InstructionTranslator::visitReturnInst(ReturnInst &retInst) {
  auto *retVal = retInst.getReturnValue();
  if (retVal != nullptr)
    retVal = retVal->stripPointerCasts();

  auto *retNode = cfg.create<tpa::ReturnCFGNode>(retVal);
  cfg.setExitNode(retNode);
  return retNode;
}

tpa::CFGNode *InstructionTranslator::visitCallInst(CallInst &callInst) {
  auto *funPtr = callInst.getCalledOperand()->stripPointerCasts();

  auto *callNode = cfg.create<tpa::CallCFGNode>(funPtr, &callInst);

  for (unsigned i = 0; i < callInst.arg_size(); ++i) {
    auto *argOp = callInst.getArgOperand(i);
    // Only pass pointer arguments to the analysis.
    if (!argOp->getType()->isPointerTy())
      continue;

    // Keep pointer-typed operands even if stripping casts would expose a
    // non-pointer (e.g., inttoptr).
    auto *arg = argOp->stripPointerCasts();
    if (!arg->getType()->isPointerTy())
      arg = argOp;

    callNode->addArgument(arg);
  }
  return callNode;
}

tpa::CFGNode *InstructionTranslator::visitInvokeInst(InvokeInst &invokeInst) {
  auto *funPtr = invokeInst.getCalledOperand()->stripPointerCasts();

  auto *callNode = cfg.create<tpa::CallCFGNode>(funPtr, &invokeInst);

  for (unsigned i = 0; i < invokeInst.arg_size(); ++i) {
    auto *argOp = invokeInst.getArgOperand(i);
    if (!argOp->getType()->isPointerTy())
      continue;

    auto *arg = argOp->stripPointerCasts();
    if (!arg->getType()->isPointerTy())
      arg = argOp;

    callNode->addArgument(arg);
  }
  return callNode;
}

tpa::CFGNode *InstructionTranslator::visitPHINode(PHINode &phiInst) {
  if (!phiInst.getType()->isPointerTy())
    return nullptr;

  auto srcs = SmallPtrSet<const Value *, 4>();
  for (unsigned i = 0; i < phiInst.getNumIncomingValues(); ++i) {
    auto *value = phiInst.getIncomingValue(i)->stripPointerCasts();
    if (isa<UndefValue>(value))
      continue;
    srcs.insert(value);
  }

  // Bug fix: if all incoming values are UndefValue (or the PHI has no
  // incoming values), srcs will be empty and createCopyNode would assert.
  // Model an all-undef PHI as a copy from UndefValue (unknown pointer).
  if (srcs.empty()) {
    srcs.insert(UndefValue::get(phiInst.getType()));
  }

  return createCopyNode(&phiInst, srcs);
}

tpa::CFGNode *InstructionTranslator::visitSelectInst(SelectInst &selectInst) {
  if (!selectInst.getType()->isPointerTy())
    return nullptr;

  auto srcs = SmallPtrSet<const Value *, 2>();
  srcs.insert(selectInst.getFalseValue()->stripPointerCasts());
  srcs.insert(selectInst.getTrueValue()->stripPointerCasts());

  return createCopyNode(&selectInst, srcs);
}

tpa::CFGNode *
InstructionTranslator::visitGetElementPtrInst(GetElementPtrInst &gepInst) {
  assert(gepInst.getType()->isPointerTy());

  auto *rawPtr = gepInst.getPointerOperand();
  auto *srcVal = rawPtr->stripPointerCasts();
  if (!srcVal->getType()->isPointerTy())
    srcVal = rawPtr;

  // Try to fold constant offset
  auto gepOffset =
      APInt(dataLayout.getPointerTypeSizeInBits(rawPtr->getType()), 0);
  if (gepInst.accumulateConstantOffset(dataLayout, gepOffset)) {
    auto offset = gepOffset.getSExtValue();

    return cfg.create<tpa::OffsetCFGNode>(&gepInst, srcVal, offset, false);
  }

  // Handle variable offset GEPs (array access).
  // Assumes canonicalized GEPs via -expand-gep pass.
  auto numOps = gepInst.getNumOperands();
  if (numOps != 2 && numOps != 3)
    llvm_unreachable(
        "Found a non-canonicalized GEP. Please run -expand-gep pass first!");

  size_t offset = dataLayout.getPointerSize();
  if (numOps == 2) {
    // Use getSourceElementType() for opaque pointer compatibility (LLVM 15+)
    offset = dataLayout.getTypeAllocSize(gepInst.getSourceElementType());
  } else {
    assert(isa<ConstantInt>(gepInst.getOperand(1)) &&
           cast<ConstantInt>(gepInst.getOperand(1))->isZero());
    // Use getResultElementType() for opaque pointer compatibility (LLVM 15+)
    // This gives us the element type after all indexing operations.
    offset = dataLayout.getTypeAllocSize(gepInst.getResultElementType());
  }

  return cfg.create<tpa::OffsetCFGNode>(&gepInst, srcVal, offset, true);
}

tpa::CFGNode *InstructionTranslator::visitIntToPtrInst(IntToPtrInst &inst) {
  assert(inst.getType()->isPointerTy());

  // Model inttoptr as producing Undef (unknown pointer).
  std::vector<const llvm::Value *> srcs = {UndefValue::get(inst.getType())};
  return cfg.create<tpa::CopyCFGNode>(&inst, std::move(srcs));
}

tpa::CFGNode *InstructionTranslator::visitBitCastInst(BitCastInst &bcInst) {
  // Bitcasts are ignored in the CFG translation because the analysis
  // strips pointer casts when looking up values.
  (void)bcInst;
  return nullptr;
}

tpa::CFGNode *
InstructionTranslator::handleUnsupportedInst(const Instruction &inst) {
  std::string instStr;
  raw_string_ostream instOS(instStr);
  instOS << inst;
  instOS.flush();
  LOG_ERROR("Unsupported instruction: {}", instStr);
  llvm_unreachable("instruction not supported");
}

tpa::CFGNode *
InstructionTranslator::visitExtractValueInst(ExtractValueInst &inst) {
  if (!inst.getType()->isPointerTy())
    return nullptr;

  // Common lowering pattern: build an aggregate with insertvalue (often from
  // undef), then extract a pointer-typed field from it.
  //
  // We try to recover the pointer value precisely by walking the insertvalue
  // chain and finding the last insertion that matches the same index path.
  const Value *agg = inst.getAggregateOperand();
  const Value *extractedPtr = nullptr;

  while (agg != nullptr) {
    if (auto *iv = dyn_cast<InsertValueInst>(agg)) {
      if (iv->getIndices() == inst.getIndices()) {
        auto *ins = iv->getInsertedValueOperand()->stripPointerCasts();
        if (ins->getType()->isPointerTy())
          extractedPtr = ins;
        break;
      }
      agg = iv->getAggregateOperand();
      continue;
    }
    break;
  }

  // If we can't recover a source, conservatively model it as an unknown
  // pointer.
  if (extractedPtr == nullptr)
    extractedPtr = UndefValue::get(inst.getType());

  auto srcs = SmallPtrSet<const Value *, 1>();
  srcs.insert(extractedPtr);
  return createCopyNode(&inst, srcs);
}
tpa::CFGNode *
InstructionTranslator::visitInsertValueInst(InsertValueInst &inst) {
  // InsertValueInst produces an aggregate type, not a pointer type directly.
  // The pointer analysis only models pointer-typed SSA values, so we can
  // safely ignore insertvalue instructions here. Any pointer-typed fields
  // inside the aggregate are recovered at their use sites via
  // visitExtractValueInst().
  //
  // Bug fix: previously this called handleUnsupportedInst() (which calls
  // llvm_unreachable) whenever the aggregate type happened to be a pointer
  // type (which is unusual but valid, e.g. a pointer-to-struct aggregate).
  // This caused an analysis crash on valid IR. Since we handle the extraction
  // side conservatively, we can safely return nullptr here.
  (void)inst;
  return nullptr;
}
tpa::CFGNode *InstructionTranslator::visitVAArgInst(VAArgInst &inst) {
  // Bug fix: previously this called handleUnsupportedInst() which calls
  // llvm_unreachable, crashing the analysis on any program that uses variadic
  // functions (va_arg). The va_arg instruction reads the next argument from a
  // va_list. For pointer analysis purposes:
  //   - If the result is not a pointer type, it is irrelevant — return nullptr.
  //   - If the result is a pointer type, we conservatively model it as an
  //     unknown pointer (UndefValue), since we cannot statically determine
  //     which variadic argument is being read.
  if (!inst.getType()->isPointerTy())
    return nullptr;

  std::vector<const llvm::Value *> srcs = {UndefValue::get(inst.getType())};
  return cfg.create<tpa::CopyCFGNode>(&inst, std::move(srcs));
}
tpa::CFGNode *
InstructionTranslator::visitExtractElementInst(ExtractElementInst &inst) {
  // Only relevant to the pointer analysis when the extracted element is a
  // pointer. Otherwise we can safely ignore it.
  if (!inst.getType()->isPointerTy())
    return nullptr;

  // Common lowering pattern: build a vector via insertelement (often from
  // undef), then extract a pointer-typed element from it.
  //
  // Try to recover the pointer value precisely when the index is constant by
  // walking the insertelement chain.
  const Value *vec = inst.getVectorOperand();
  const Value *extractedPtr = nullptr;

  auto *idxC = dyn_cast<ConstantInt>(inst.getIndexOperand());
  if (idxC != nullptr) {
    const uint64_t targetIdx = idxC->getZExtValue();

    while (vec != nullptr) {
      if (auto *ie = dyn_cast<InsertElementInst>(vec)) {
        auto *ieIdxC = dyn_cast<ConstantInt>(ie->getOperand(2));
        if (ieIdxC != nullptr && ieIdxC->getZExtValue() == targetIdx) {
          auto *ins = ie->getOperand(1)->stripPointerCasts();
          if (ins->getType()->isPointerTy())
            extractedPtr = ins;
          break;
        }
        vec = ie->getOperand(0);
        continue;
      }
      break;
    }
  }

  // If we can't recover a source, conservatively model it as an unknown
  // pointer.
  if (extractedPtr == nullptr)
    extractedPtr = UndefValue::get(inst.getType());

  auto srcs = SmallPtrSet<const Value *, 1>();
  srcs.insert(extractedPtr);
  return createCopyNode(&inst, srcs);
}
tpa::CFGNode *
InstructionTranslator::visitInsertElementInst(InsertElementInst &inst) {
  // Vectors are not memory locations. The pointer analysis only needs to model
  // pointer-typed SSA values. We recover pointer elements at use sites (e.g.,
  // extractelement), so insertelement itself can be ignored.
  (void)inst;
  return nullptr;
}
tpa::CFGNode *
InstructionTranslator::visitShuffleVectorInst(ShuffleVectorInst &inst) {
  // Similar to insertelement: pointer-typed values extracted from shuffled
  // vectors are handled conservatively at the extraction site.
  (void)inst;
  return nullptr;
}
tpa::CFGNode *InstructionTranslator::visitLandingPadInst(LandingPadInst &inst) {
  // `landingpad` produces an aggregate { i8*, i32 } (or similar), not a
  // pointer-typed SSA value. The pointer analysis only models pointer-typed
  // SSA values; any uses that extract a pointer field are handled
  // conservatively by `visitExtractValueInst()` (fallback to unknown pointer
  // when needed).
  //
  // So we can safely ignore `landingpad` here to avoid crashing on IR with EH.
  (void)inst;
  return nullptr;
}

} // namespace tpa