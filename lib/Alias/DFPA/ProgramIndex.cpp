#include "Alias/DFPA/ProgramIndex.h"

#include <algorithm>

#include <llvm/IR/Constants.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace dfpa;

namespace {

static std::string stripSpaces(std::string S) {
  S.erase(std::remove(S.begin(), S.end(), ' '), S.end());
  return S;
}

static bool isHeapLikeAllocator(const CallBase *CB) {
  auto *Callee = CB->getCalledFunction();
  if (!Callee)
    return false;
  StringRef Name = Callee->getName();
  return Name == "malloc" || Name == "calloc" || Name == "realloc" ||
         Name == "_Znwm" || Name == "_Znam";
}

} // namespace

ProgramIndex::ProgramIndex(Module &M) : module_(M) {
  collectObjects();
  collectFunctions();
}

const AbstractObject *ProgramIndex::lookupObject(Value *V) const {
  auto It = object_lookup_.find(V);
  if (It == object_lookup_.end())
    return nullptr;
  return &objects_[It->second];
}

const AbstractObject *ProgramIndex::lookupFormalObject(
    const Argument *Arg) const {
  auto It = formal_object_lookup_.find(Arg);
  if (It == formal_object_lookup_.end())
    return nullptr;
  return &objects_[It->second];
}

std::string ProgramIndex::normalizeSignature(FunctionType *FTy) const {
  std::string Text;
  raw_string_ostream OS(Text);
  FTy->print(OS);
  OS.flush();
  return stripSpaces(Text);
}

std::string ProgramIndex::getSignature(const Function *F) const {
  return normalizeSignature(F->getFunctionType());
}

std::string ProgramIndex::getSignature(const CallBase *CB) const {
  return normalizeSignature(CB->getFunctionType());
}

void ProgramIndex::collectObjects() {
  unsigned NextId = 0;
  for (GlobalVariable &GV : module_.globals()) {
    objects_.push_back(
        {NextId, ObjectKind::Global, &GV, nullptr, GV.getName().str()});
    object_lookup_[&GV] = NextId++;
    if (GV.hasInitializer())
      globals_with_initializers_.push_back(&GV);
  }

  for (Function &F : module_) {
    for (Argument &Arg : F.args()) {
      if (!Arg.getType()->isPointerTy())
        continue;
      objects_.push_back({NextId, ObjectKind::FormalIn, &Arg, &F,
                          (F.getName() + ".arg" + Twine(Arg.getArgNo())).str()});
      formal_object_lookup_[&Arg] = NextId;
      object_lookup_[&Arg] = NextId++;
    }

    if (F.isDeclaration())
      continue;

    for (Instruction &I : instructions(F)) {
      if (auto *AI = dyn_cast<AllocaInst>(&I)) {
        objects_.push_back({NextId, ObjectKind::Alloca, AI, &F,
                            AI->hasName() ? AI->getName().str()
                                          : ("alloca" + Twine(NextId)).str()});
        object_lookup_[AI] = NextId++;
        continue;
      }

      auto *CB = dyn_cast<CallBase>(&I);
      if (!CB || !isHeapLikeAllocator(CB))
        continue;

      objects_.push_back({NextId, ObjectKind::Heap, CB, &F,
                          CB->hasName() ? CB->getName().str()
                                        : ("heap" + Twine(NextId)).str()});
      object_lookup_[CB] = NextId++;
    }
  }
}

void ProgramIndex::collectFunctions() {
  for (Function &F : module_) {
    if (F.hasAddressTaken() && !F.isIntrinsic())
      address_taken_by_signature_[getSignature(&F)].insert(&F);

    if (F.isDeclaration())
      continue;

    for (Instruction &I : instructions(F)) {
      if (auto *RI = dyn_cast<ReturnInst>(&I))
        returns_[&F].push_back(RI);

      auto *CB = dyn_cast<CallBase>(&I);
      if (!CB)
        continue;
      if (CB->isIndirectCall())
        indirect_calls_.push_back(CB);
      else if (Function *Callee = CB->getCalledFunction())
        if (!Callee->isIntrinsic())
          direct_callers_[Callee].push_back(CB);

      if (Function *Callee = CB->getCalledFunction())
        if (Callee->getName().startswith("llvm.memcpy") ||
            Callee->getName().startswith("llvm.memmove"))
          mem_transfers_.push_back(CB);
    }

    for (Instruction &I : instructions(F))
      if (auto *SI = dyn_cast<StoreInst>(&I))
        stores_.push_back(SI);
  }
}
