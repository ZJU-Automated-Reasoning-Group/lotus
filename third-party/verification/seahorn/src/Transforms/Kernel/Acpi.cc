#include "seahorn/Transforms/Kernel/SlimDown.hh"

#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"

// indices of fields in struct acpi_driver
#define ACPI_DEVICE_OPS_INDEX 4
// indices of fields in struct acpi_device_ops
#define ACPI_OP_ADD_INDEX 0

using namespace llvm;

namespace seahorn {

class AcpiSetup : public ModulePass {
public:
  static char ID;

  AcpiSetup(StringRef acpiDriver)
      : ModulePass(ID), acpiDriverName(acpiDriver) {}

  bool runOnModule(Module &M) override {
    fillPlaceholder(M);
    // GlobalVariable *acpiRoot = M.getNamedGlobal("acpi_gbl_root_table_list");
    // slimDown(M, acpiRoot);
    return true;
  }

  virtual StringRef getPassName() const override { return "AcpiSetup"; }

private:
  StringRef acpiDriverName;

  Function *getAcpiOpAddFns(Module &M) {
    GlobalVariable *driver = M.getGlobalVariable(acpiDriverName, true);
    Constant *init = driver->getInitializer();
    ConstantStruct *cs = cast<ConstantStruct>(init);
    ConstantStruct *acpiDeviceOps =
        cast<ConstantStruct>(cs->getOperand(ACPI_DEVICE_OPS_INDEX));
    Function *acpiOpAdd =
        cast<Function>(acpiDeviceOps->getOperand(ACPI_OP_ADD_INDEX));
    return acpiOpAdd;
  }

  void fillPlaceholder(Module &M) {
    Function *placeholder = M.getFunction("__PLACEHOLDER_acpi_driver_add");
    if (!placeholder) {
      errs() << "Placeholder not found\n";
      std::exit(1);
    }
    Function *acpiOpAddFn = getAcpiOpAddFns(M);
    if (!acpiOpAddFn) {
      errs() << "ACPI driver initialization function not found\n";
      std::exit(1);
    }
    Type *addArgType = acpiOpAddFn->getArg(0)->getType();
    for (User *u : placeholder->users()) {
      if (CallInst *call = dyn_cast<CallInst>(u)) {
        IRBuilder<> B(call);
        Value *arg = call->getArgOperand(0);
        if (arg->getType() != addArgType)
          arg = B.CreateBitCast(arg, addArgType);
        CallInst *newCall =
            B.CreateCall(acpiOpAddFn->getFunctionType(), acpiOpAddFn, arg);
        call->replaceAllUsesWith(newCall);
        call->dropAllReferences();
        call->eraseFromParent();
      }
    }
  }
};

char AcpiSetup::ID = 0;

Pass *createAcpiSetupPass(StringRef acpiDrivers) {
  return new AcpiSetup(acpiDrivers);
}
} // namespace seahorn