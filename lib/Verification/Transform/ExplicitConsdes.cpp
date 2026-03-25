#include "Verification/Transform/ExplicitConsdes.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <vector>

using namespace llvm;

namespace {

struct FunctionEntry {
  uint32_t priority;
  Function *function;
  Value *param;
};

static bool priorityAsc(const FunctionEntry &a, const FunctionEntry &b) {
  return a.priority < b.priority;
}

static bool priorityDesc(const FunctionEntry &a, const FunctionEntry &b) {
  return a.priority > b.priority;
}

void fillFromInitializer(std::vector<FunctionEntry> &target, Constant *init) {
  auto *ty = init->getType();
  if (!ty->isArrayTy()) {
    errs() << "explicit-consdes: unexpected type of global var "
              "initializer\n";
    return;
  }

  auto *inner = ty->getArrayElementType();
  if (inner->getStructNumElements() != 3 ||
      !inner->getStructElementType(0)->isIntegerTy() ||
      !inner->getStructElementType(1)
           ->getPointerElementType()
           ->isFunctionTy() ||
      !inner->getStructElementType(2)->isPointerTy()) {
    errs() << "explicit-consdes: unexpected type of element in global "
              "var initializer\n";
    return;
  }

  for (unsigned i = 0; i < init->getType()->getArrayNumElements(); ++i) {
    // type { i32, void ()*, i8* }
    auto *elem = init->getAggregateElement(i);

    FunctionEntry entry{};
    entry.priority =
        dyn_cast<ConstantInt>(elem->getAggregateElement(0u))->getZExtValue();
    entry.function = dyn_cast<Function>(elem->getAggregateElement(1));
    entry.param = elem->getAggregateElement(2);
    target.push_back(std::move(entry));
  }
}

void markUnused(GlobalVariable *gv) {
  if (!gv)
    return;

  gv->setName(gv->getName() + "_unused");
}

void insertCalls(std::vector<FunctionEntry> &entries, Instruction *before) {
  if (entries.empty() || !before)
    return;

  Instruction *after = nullptr;

  for (auto &entry : entries) {
    if (!entry.function)
      continue;

    auto *call = CallInst::Create(entry.function->getFunctionType(),
                                  entry.function, "", before);

    call->setDebugLoc(before->getDebugLoc());

    if (after) {
      call->insertAfter(after);
    } else {
      call->insertBefore(before);
    }
    after = call;
  }
}

bool isExit(const Function *f) {
  const Type *ty = f->getType();
  if (ty->isPointerTy())
    ty = ty->getPointerElementType();

  return f->hasName() && f->getName() == "exit" &&
         ty->getFunctionNumParams() == 1 &&
         ty->getFunctionParamType(0)->isIntegerTy();
}

bool isMarkExit(const Function *f) {
  const Type *ty = f->getType();
  if (ty->isPointerTy())
    ty = ty->getPointerElementType();
  return f->hasName() && f->getName() == "__INSTR_mark_exit" &&
         ty->getFunctionNumParams() == 0;
}

void warnIfExitIsUsed(const Instruction &inst) {
  // inst -> [operands] -> [only function pointers] -> [only exit] -> is_empty
  for (const auto &op : inst.operands()) {
    const auto *val = op.get();
    if (!val)
      continue;

    const auto *ty = val->getType();

    if (!ty->isPointerTy())
      continue;

    if (!ty->getPointerElementType()->isFunctionTy())
      continue;

    const auto *f = dyn_cast<Function>(val);
    if (!f || !isExit(f))
      continue;

    errs() << "explicit-consdes: warning: indirect call of exit is possible in "
              "the program\n";
  }
}

} // namespace

namespace lotus {
namespace verification {
namespace transform {

char ExplicitConsdesPass::ID = 0;

bool ExplicitConsdesPass::runOnModule(Module &mod) {
  std::vector<FunctionEntry> ctors, dtors;

  GlobalVariable *dtorsVar = mod.getNamedGlobal("llvm.global_dtors");
  GlobalVariable *ctorsVar = mod.getNamedGlobal("llvm.global_ctors");

  if (!ctorsVar && !dtorsVar)
    return false;

  if (dtorsVar) {
    fillFromInitializer(dtors, dtorsVar->getInitializer());
  }

  if (ctorsVar) {
    fillFromInitializer(ctors, ctorsVar->getInitializer());
  }

  // sort them to call order
  std::sort(ctors.begin(), ctors.end(), priorityAsc);
  std::sort(dtors.begin(), dtors.end(), priorityDesc);

  // call constructors at the beginning of main
  auto *main = mod.getFunction("main");
  if (!main || main->isDeclaration()) {
    // No main function, skip constructor insertion
    if (!ctors.empty() || !dtors.empty()) {
      errs() << "explicit-consdes: warning: found ctors/dtors but no main "
                "function\n";
    }
    return false;
  }

  auto *ctorsBefore = main->getEntryBlock().getFirstNonPHIOrDbgOrLifetime();
  if (ctorsBefore)
    insertCalls(ctors, ctorsBefore);

  std::vector<Instruction *> dtorsBefore;

  // search for returns in main
  for (auto &block : *main) {
    if (isa<ReturnInst>(block.getTerminator())) {
      dtorsBefore.push_back(block.getTerminator());
    }
  }

  // if program has __INSTR_mark_exit, we can't insert instruction between it
  // and exit
  bool hasMarkExit = (mod.getFunction("__INSTR_mark_exit") != nullptr);

  // and finally search for calls to exit in whole program
  for (auto &func : mod.functions()) {
    for (auto &inst : instructions(func)) {
      auto *call = dyn_cast<CallInst>(&inst);
      if (!call) {
        warnIfExitIsUsed(inst);
        continue;
      }

      Function *called = call->getCalledFunction();
      if (!called) // indirect call
        continue;

      if (!isExit(called))
        continue;

      if (!hasMarkExit) {
        dtorsBefore.push_back(call);
      } else {
        // look for __INSTR_mark_exit before this call
        auto *prev = inst.getPrevNonDebugInstruction();
        auto *markCall = prev ? dyn_cast<CallInst>(prev) : nullptr;
        if (!markCall)
          continue;

        Function *markCalled = markCall->getCalledFunction();
        if (!markCalled) // indirect call
          continue;

        if (!isMarkExit(markCalled))
          continue;

        dtorsBefore.push_back(markCall);
      }
    }
  }

  for (auto *before : dtorsBefore) {
    insertCalls(dtors, before);
  }

  markUnused(ctorsVar);
  markUnused(dtorsVar);

  return true;
}

} // namespace transform
} // namespace verification
} // namespace lotus

static RegisterPass<lotus::verification::transform::ExplicitConsdesPass>
    X("explicit-consdes",
      "Insert explicit calls of module constructors and destructors", false,
      false);

namespace lotus {
namespace verification {
namespace transform {

llvm::Pass *createExplicitConsdesPass() { return new ExplicitConsdesPass(); }

} // namespace transform
} // namespace verification
} // namespace lotus
