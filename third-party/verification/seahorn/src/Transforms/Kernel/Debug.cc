#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Pass.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Scalar.h"

#include "seahorn/Transforms/Kernel/Util.hh"

using namespace llvm;

namespace seahorn {

class Debug : public ModulePass {
public:
  static char ID;

  Debug(StringRef outLLFileName)
      : ModulePass(ID), outLLFileName(outLLFileName) {}

  bool runOnModule(Module &m) override {
    stubFunctionCalls(m);

    legacy::PassManager pm;
    pm.add(createAggressiveDCEPass());
    pm.add(createGlobalDCEPass());
    pm.run(m);

    unsigned int counter = 0;
    if (!outLLFileName.empty()) {
      std::error_code ec;
      raw_fd_ostream fileOs(outLLFileName, ec);
      m.setModuleInlineAsm("");
      m.print(fileOs, nullptr);
    }

    for (Function &F : m) {
      if (F.isDeclaration() || !F.hasName())
        continue;
      for (Instruction &inst : instructions(F)) {
        if (CallInst *call = dyn_cast<CallInst>(&inst)) {
          if (InlineAsm *inlineAsm =
                  dyn_cast<InlineAsm>(call->getCalledOperand())) {
            StringRef name = F.getName();
            errs() << "In func " << name.str() << "\n";
            errs() << "asm: " << inlineAsm->getAsmString() << "\n";
            errs() << "constraints " << inlineAsm->getConstraintString()
                   << "\n";
            counter++;
          }
        }
      }
    }
    errs() << "Total number of inline asm instructions: " << counter << "\n";
    verify(m);
    return false;
  }

  virtual StringRef getPassName() const override { return "KernelDebug"; }

private:
  StringRef outLLFileName;

  void aggregateUsers(User *user, DenseSet<StringRef> &s, StringRef prev) {
    StringRef name = "";
    if (Instruction *inst = dyn_cast<Instruction>(user)) {
      name = inst->getFunction()->getName();
      user = inst->getFunction();
    } else if (GlobalVariable *g = dyn_cast<GlobalVariable>(user)) {
      name = g->getName();
    }
    bool skip = false;
    if (!name.empty()) {
      skip = !s.insert(name).second;
      if (!skip) {
        errs() << "inserting " << name << " because of " << prev << "\n";
      }
    }
    if (skip)
      return;
    StringRef newPrev = prev;
    if (!name.empty()) {
      newPrev = name;
    }
    for (User *u : user->users())
      aggregateUsers(u, s, newPrev);
  }

  void verify(Module &M) {
    if (verifyModule(M, &errs())) {
      for (Function &f : M) {
        if (verifyFunction(f, &errs())) {
          errs() << "Function " << f.getName() << " verification failed\n";
          f.dump();
        }
      }
      errs() << "Module verification failed\n";
    } else {
      errs() << "Module verification Ok\n";
    }
  }

  void stubFunctionCalls(Module &m) {
    StringRef names[] = {
        "of_irq_parse_one",
        "__of_parse_phandle_with_args",
        "of_mdiobus_register",
    };
    for (StringRef name : names) {
      if (Function *f = m.getFunction(name)) {
        for (CallInst *call : getCalls(f)) {
          if (!f->getReturnType()->isVoidTy()) {
            Constant *zero = Constant::getNullValue(f->getReturnType());
            call->replaceAllUsesWith(zero);
          }
          call->eraseFromParent();
        }
        f->eraseFromParent();
      }
    }
  }
};

char Debug::ID = 0;

Pass *createKernelDebugPass(StringRef outLLFileName) {
  return new Debug(outLLFileName);
};
} // namespace seahorn