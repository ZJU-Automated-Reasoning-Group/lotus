#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Pass.h"

#include "seahorn/Analysis/SeaBuiltinsInfo.hh"

using namespace llvm;

namespace seahorn {

class AssumeNonNull : public ModulePass {
public:
  static char ID;

  AssumeNonNull() : ModulePass(ID) {}

  bool runOnModule(Module &m) override {
    SeaBuiltinsInfo &sbi = getAnalysis<SeaBuiltinsInfoWrapperPass>().getSBI();
    Function *assumeNotFn = sbi.mkSeaBuiltinFn(SeaBuiltinsOp::ASSUME_NOT, m);

    for (Function &f : m) {
      for (Instruction &inst : instructions(f)) {
        if (GEPOperator *gep = dyn_cast<GEPOperator>(&inst)) {
          if (isContainerOfGEP(gep)) {
            IRBuilder<> b(inst.getParent()->getTerminator());
            Value *isNull = b.CreateIsNull(gep);
            b.CreateCall(assumeNotFn, isNull);
          }
        }
      }
    }
    return false;
  }

  virtual StringRef getPassName() const override { return "AssertNonNull"; }

  void getAnalysisUsage(AnalysisUsage &au) const override {
    au.addRequired<SeaBuiltinsInfoWrapperPass>();
    au.setPreservesAll();
  }

private:
  // If the first index of a GEP is negative, then assume this GEP is a result
  // of a container_of macro.
  bool isContainerOfGEP(const GEPOperator *gep) {
    const Value *firstIndex = gep->getOperand(1);
    if (const ConstantInt *c = dyn_cast<ConstantInt>(firstIndex)) {
      return c->isNegative();
    }
    return false;
  }
};

char AssumeNonNull::ID = 0;

Pass *createAssumeNonNullPass() { return new AssumeNonNull(); }

}; // namespace seahorn