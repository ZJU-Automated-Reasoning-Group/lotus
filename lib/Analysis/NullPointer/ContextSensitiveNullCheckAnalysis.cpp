#include "Analysis/NullPointer/ContextSensitiveNullCheckAnalysis.h"

#include "Utils/LLVM/RecursiveTimer.h"

using namespace llvm;

char ContextSensitiveNullCheckAnalysis::ID = 0;
static RegisterPass<ContextSensitiveNullCheckAnalysis>
    X("csnca", "context-sensitive null check analysis.");

ContextSensitiveNullCheckAnalysis::ContextSensitiveNullCheckAnalysis()
    : ModulePass(ID) {}

ContextSensitiveNullCheckAnalysis::~ContextSensitiveNullCheckAnalysis() =
    default;

void ContextSensitiveNullCheckAnalysis::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<ContextSensitiveNullFlowAnalysis>();
}

bool ContextSensitiveNullCheckAnalysis::runOnModule(Module &M) {
  RecursiveTimer Timer("Running Context-Sensitive NullCheckAnalysis");
  NFA = &getAnalysis<ContextSensitiveNullFlowAnalysis>();
  (void)M;
  return false;
}

bool ContextSensitiveNullCheckAnalysis::mayNull(Value *Ptr, Instruction *Inst) {
  if (!Ptr || !Inst || !Ptr->getType()->isPointerTy() || !NFA) {
    return true;
  }

  auto Contexts = NFA->getReachableContexts(Inst);
  if (Contexts.empty()) {
    return true;
  }

  for (const auto &Ctx : Contexts) {
    if (!NFA->notNull(Ptr, Inst, Ctx)) {
      return true;
    }
  }
  return false;
}

bool ContextSensitiveNullCheckAnalysis::mayNull(Value *Ptr, Instruction *Inst,
                                                const Context &Ctx) {
  if (!Ptr || !Inst || !Ptr->getType()->isPointerTy() || !NFA) {
    return true;
  }
  return !NFA->notNull(Ptr, Inst, Ctx);
}

std::string
ContextSensitiveNullCheckAnalysis::getContextString(const Context &Ctx) const {
  return Ctx.str();
}

std::vector<ContextSensitiveNullCheckAnalysis::Context>
ContextSensitiveNullCheckAnalysis::getReachableContexts(Instruction *Inst) const {
  if (!NFA) {
    return {};
  }
  return NFA->getReachableContexts(Inst);
}
