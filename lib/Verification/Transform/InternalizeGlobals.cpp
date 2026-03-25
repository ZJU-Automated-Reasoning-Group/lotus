#include "Verification/Transform/InternalizeGlobals.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace {

bool CloneMetadata(const Instruction *i1, Instruction *i2) {
  if (i1->hasMetadata()) {
    i2->setDebugLoc(i1->getDebugLoc());
    return true;
  }
  return false;
}

} // namespace

namespace lotus {
namespace verification {
namespace transform {

char InternalizeGlobalsPass::ID = 0;

static Function *get_verifier_make_nondet(Module *M) {
  LLVMContext &Ctx = M->getContext();
  auto C = M->getOrInsertFunction(
      "__VERIFIER_make_nondet", Type::getVoidTy(Ctx),
      Type::getInt8PtrTy(Ctx),                                         // addr
      Type::getIntNTy(Ctx, M->getDataLayout().getPointerSizeInBits()), // nbytes
      Type::getInt8PtrTy(Ctx)                                          // name
  );
  return cast<Function>(C.getCallee());
}

static Type *get_size_t(Module *M) {
  DataLayout DL(M->getDataLayout());
  LLVMContext &Ctx = M->getContext();
  if (DL.getPointerSizeInBits() > 32)
    return Type::getInt64Ty(Ctx);
  else
    return Type::getInt32Ty(Ctx);
}

bool InternalizeGlobalsPass::runOnModule(Module &M) {
  bool modified = false;
  LLVMContext &Ctx = M.getContext();
  std::unique_ptr<DataLayout> DL(new DataLayout(M.getDataLayout()));

  Function *main = M.getFunction("main");
  if (!main || main->isDeclaration())
    return false;

  Function *vms = get_verifier_make_nondet(&M);
  Type *size_t_Ty = get_size_t(&M);

  for (GlobalVariable &GV : M.globals()) {
    if (GV.hasInitializer())
      continue;

    // Skip standard library globals
    if (GV.hasName()) {
      StringRef name = GV.getName();
      if (name.equals("stdin") || name.equals("stderr") ||
          name.equals("stdout"))
        continue;
    }

    // GV is a pointer to some memory, we want the size of the memory
    Type *Ty = GV.getType()->getContainedType(0);
    if (!Ty->isSized()) {
      errs() << "ERROR: failed making global variable symbolic "
                "(type is unsized): "
             << GV << "\n";
      continue;
    }

    // What memory will be made symbolic
    Value *memory = &GV;

    // The global is a pointer, so create an object that it can point to
    if (Ty->isPointerTy()) {
      if (!Ty->getContainedType(0)->isSized()) {
        errs() << "ERROR: failed making global variable symbolic "
                  "(referenced type is unsized): "
               << GV << "\n";
        continue;
      }

      Constant *init = Constant::getNullValue(Ty->getContainedType(0));
      GlobalVariable *pointedG =
          new GlobalVariable(M, Ty->getContainedType(0), false /*constant*/,
                             GlobalVariable::PrivateLinkage, init);
      GV.setInitializer(pointedG);

      memory = pointedG;
      Ty = Ty->getContainedType(0);
    } else {
      // Set initializer (will be overwritten at beginning of main)
      if (GV.hasName() && GV.getName().equals("optind")) {
        GV.setInitializer(ConstantInt::get(Type::getInt32Ty(Ctx), 1));
      } else {
        GV.setInitializer(Constant::getNullValue(GV.getValueType()));
      }
    }

    // Skip optind and optarg (keep their initializers)
    if (GV.hasName()) {
      StringRef name = GV.getName();
      if (name.equals("optind") || name.equals("optarg"))
        continue;
    }

    CastInst *CastI =
        CastInst::CreatePointerCast(memory, Type::getInt8PtrTy(Ctx));

    std::vector<Value *> args;
    args.push_back(CastI);
    args.push_back(ConstantInt::get(size_t_Ty, DL->getTypeAllocSize(Ty)));
    std::string nameStr = GV.hasName() ? GV.getName().str() : "extern-global";
    Constant *name = ConstantDataArray::getString(Ctx, nameStr);
    GlobalVariable *nameG =
        new GlobalVariable(M, name->getType(), true /*constant*/,
                           GlobalVariable::PrivateLinkage, name);
    args.push_back(
        ConstantExpr::getPointerCast(nameG, Type::getInt8PtrTy(Ctx)));
    CallInst *CI = CallInst::Create(vms, args);

    BasicBlock &block = main->getEntryBlock();
    Instruction &Inst = *(block.begin());
    CastI->insertBefore(&Inst);
    CI->insertBefore(&Inst);

    CloneMetadata(&Inst, CI);

    modified = true;

    GV.setExternallyInitialized(false);
    GV.setConstant(false);
    errs() << "Made global variable '" << GV.getName() << "' non-extern\n";
  }

  return modified;
}

} // namespace transform
} // namespace verification
} // namespace lotus

static llvm::RegisterPass<
    lotus::verification::transform::InternalizeGlobalsPass>
    X("internalize-globals",
      "Internalize and make non-deterministic external globals");

namespace lotus {
namespace verification {
namespace transform {

llvm::Pass *createInternalizeGlobalsPass() {
  return new InternalizeGlobalsPass();
}

} // namespace transform
} // namespace verification
} // namespace lotus
