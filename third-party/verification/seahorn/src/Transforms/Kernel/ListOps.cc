#include "llvm/IR/Module.h"
#include "llvm/Pass.h"

#include "seahorn/Transforms/Kernel/Util.hh"

using namespace llvm;

namespace seahorn {

enum class OpKind {
  FileOperations,
  NetDeviceOps,
  PlatformDriver,
  DsaSwitchOps,
};

class ListOps : public ModulePass {
public:
  static char ID;

  ListOps(ArrayRef<std::string> ops) : ModulePass(ID), ops(ops) {}

  bool runOnModule(Module &m) override {
    LLVMContext &ctx = m.getContext();
    for (const std::string &op : ops) {
      StructType *s = StructType::getTypeByName(ctx, "struct." + op);
      if (!s) {
        errs() << "No struct type found for " << op << "\n";
        continue;
      }
      const SmallVector<StringRef> &names = getOpNames(m, s);
      for (StringRef name : names) {
        outs() << op << " " << name << "\n";
      }
    }
    return false;
  }

  virtual StringRef getPassName() const override { return "ListOps"; }

private:
  ArrayRef<std::string> ops;

  SmallVector<StringRef> getOpNames(Module &m, StructType *targetType) {
    SmallVector<StringRef> names;
    for (GlobalVariable &gv : m.globals()) {
      Type *ty = gv.getValueType();
      bool isTargetOp = equivTypes(ty, targetType);
      if (ty->isPointerTy())
        isTargetOp |= equivTypes(ty->getPointerElementType(), targetType);
      if (isTargetOp) {
        names.push_back(gv.getName());
      }
    }
    std::sort(names.begin(), names.end());
    return names;
  }
};

char ListOps::ID = 0;
Pass *createListOpsPass(ArrayRef<std::string> ops) { return new ListOps(ops); }

} // namespace seahorn