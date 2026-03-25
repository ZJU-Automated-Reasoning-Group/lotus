#include "Verification/Transform/InstrumentAlloc.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"

using namespace llvm;

namespace lotus {
namespace verification {
namespace transform {

char InstrumentAllocPass::ID = 0;
char InstrumentAllocNeverFailsPass::ID = 0;

static void replace_malloc(Module *M, CallInst *CI, bool never_fails) {
  FunctionCallee F;
  if (never_fails) {
    F = M->getOrInsertFunction("__VERIFIER_malloc0", CI->getType(),
                               CI->getOperand(0)->getType());
  } else {
    F = M->getOrInsertFunction("__VERIFIER_malloc", CI->getType(),
                               CI->getOperand(0)->getType());
  }

  Function *Malloc = cast<Function>(F.getCallee());

  std::vector<Value *> args;
  args.push_back(CI->getOperand(0));

  CallInst *new_CI = CallInst::Create(Malloc, args);

  SmallVector<std::pair<unsigned, MDNode *>, 8> metadata;
  CI->getAllMetadata(metadata);
  // Copy the metadata
  for (auto &md : metadata)
    new_CI->setMetadata(md.first, md.second);
  // Copy the attributes
  new_CI->setAttributes(CI->getAttributes());

  new_CI->insertBefore(CI);
  CI->replaceAllUsesWith(new_CI);
  CI->eraseFromParent();
}

static void replace_calloc(Module *M, CallInst *CI, bool never_fails) {
  FunctionCallee F;
  if (never_fails) {
    F = M->getOrInsertFunction("__VERIFIER_calloc0", CI->getType(),
                               CI->getOperand(0)->getType(),
                               CI->getOperand(1)->getType());
  } else {
    F = M->getOrInsertFunction("__VERIFIER_calloc", CI->getType(),
                               CI->getOperand(0)->getType(),
                               CI->getOperand(1)->getType());
  }

  Function *Calloc = cast<Function>(F.getCallee());

  std::vector<Value *> args;
  args.push_back(CI->getOperand(0));
  args.push_back(CI->getOperand(1));

  CallInst *new_CI = CallInst::Create(Calloc, args);

  SmallVector<std::pair<unsigned, MDNode *>, 8> metadata;
  CI->getAllMetadata(metadata);
  // Copy the metadata
  for (auto &md : metadata)
    new_CI->setMetadata(md.first, md.second);
  // Copy the attributes
  new_CI->setAttributes(CI->getAttributes());

  new_CI->insertBefore(CI);
  CI->replaceAllUsesWith(new_CI);
  CI->eraseFromParent();
}

static bool instrument_alloc(Function &F, bool never_fails) {
  // Do not run on __VERIFIER and __INSTR functions
  const auto &fname = F.getName();
  if (fname.startswith("__VERIFIER_") || fname.startswith("__INSTR_"))
    return false;

  bool modified = false;
  Module *M = F.getParent();

  for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E;) {
    Instruction *ins = &*I;
    ++I;
    if (CallInst *CI = dyn_cast<CallInst>(ins)) {
      if (CI->isInlineAsm())
        continue;

      const Value *val = CI->getCalledOperand()->stripPointerCasts();
      const Function *callee = dyn_cast<Function>(val);
      if (!callee || callee->isIntrinsic())
        continue;

      if (!callee->hasName())
        continue;

      StringRef name = callee->getName();

      if (name.equals("malloc")) {
        replace_malloc(M, CI, never_fails);
        modified = true;
      } else if (name.equals("calloc")) {
        replace_calloc(M, CI, never_fails);
        modified = true;
      }
    }
  }
  return modified;
}

bool InstrumentAllocPass::runOnFunction(Function &F) {
  return instrument_alloc(F, false /* never fails */);
}

bool InstrumentAllocNeverFailsPass::runOnFunction(Function &F) {
  return instrument_alloc(F, true /* never fails */);
}

} // namespace transform
} // namespace verification
} // namespace lotus

static llvm::RegisterPass<lotus::verification::transform::InstrumentAllocPass>
    X("instrument-alloc",
      "Replace calls to malloc and calloc with verifier functions");

static llvm::RegisterPass<
    lotus::verification::transform::InstrumentAllocNeverFailsPass>
    Y("instrument-alloc-nf",
      "Replace calls to malloc/calloc with verifier functions (never fails)");

namespace lotus {
namespace verification {
namespace transform {

llvm::Pass *createInstrumentAllocPass() { return new InstrumentAllocPass(); }

llvm::Pass *createInstrumentAllocNeverFailsPass() {
  return new InstrumentAllocNeverFailsPass();
}

} // namespace transform
} // namespace verification
} // namespace lotus
