#include "Verification/Analysis/ClassifyInstructions.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace lotus {
namespace verification {
namespace analysis {

char ClassifyInstructionsPass::ID = 0;

namespace {
struct ClassifyState {
  bool stack_array = false;
  bool stack_var_array = false;
  bool has_malloc = false, has_calloc = false, has_realloc = false;
  bool has_big_malloc = false, has_var_malloc = false;
  bool bit_shift = false, bit_logic = false;
};
} // namespace

bool ClassifyInstructionsPass::runOnFunction(Function &F) {
  if (F.isDeclaration())
    return false;

  static ClassifyState state;

  for (auto &B : F) {
    for (auto &I : B) {
      if (auto *AI = dyn_cast<AllocaInst>(&I)) {
        if (AI->isArrayAllocation()) {
          state.stack_array = true;
          state.stack_var_array = true;
        }
        if (AI->getAllocatedType()->isArrayTy()) {
          state.stack_array = true;
        }
      } else if (auto *CI = dyn_cast<CallInst>(&I)) {
        auto *CV = CI->getCalledOperand()->stripPointerCasts();
        if (CV) {
          const auto &name = cast<Function>(CV)->getName();
          if (name.equals("malloc")) {
            state.has_malloc = true;
            if (auto *C = dyn_cast<ConstantInt>(CI->getOperand(0))) {
              if (C->getZExtValue() > 8)
                state.has_big_malloc = true;
            } else
              state.has_var_malloc = true;
          } else if (name.equals("calloc"))
            state.has_calloc = true;
          else if (name.equals("realloc"))
            state.has_realloc = true;
          else if (name.equals("alloca"))
            state.stack_var_array = true;
        }
      } else {
        switch (I.getOpcode()) {
        case Instruction::And:
        case Instruction::Or:
        case Instruction::Xor:
          state.bit_logic = true;
          break;
        case Instruction::Shl:
        case Instruction::AShr:
        case Instruction::LShr:
          state.bit_shift = true;
        }
      }
    }
  }

  return false;
}

bool ClassifyInstructionsPass::doFinalization(Module &M) {
  static ClassifyState state;

  if (state.stack_array)
    errs() << "array on stack\n";
  if (state.stack_var_array)
    errs() << "alloca or variable-length array\n";
  if (state.has_malloc) {
    errs() << "calls malloc\n";
    if (state.has_big_malloc)
      errs() << "  > 8b malloc\n";
    if (state.has_var_malloc)
      errs() << "  var-sized malloc\n";
  }
  if (state.has_calloc)
    errs() << "calls calloc\n";
  if (state.has_realloc)
    errs() << "calls realloc\n";
  if (state.bit_logic)
    errs() << "bit-wise operations\n";
  if (state.bit_shift)
    errs() << "bit-shift operations\n";

  // Reset state
  state = ClassifyState{};

  return false;
}

} // namespace analysis
} // namespace verification
} // namespace lotus

static llvm::RegisterPass<
    lotus::verification::analysis::ClassifyInstructionsPass>
    X("classify-instructions", "Print statistics from module");

namespace lotus {
namespace verification {
namespace analysis {

llvm::Pass *createClassifyInstructionsPass() {
  return new ClassifyInstructionsPass();
}

} // namespace analysis
} // namespace verification
} // namespace lotus
