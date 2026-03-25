#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>

namespace dfpa {

enum class ObjectKind {
  Global,
  Alloca,
  Heap,
  FormalIn,
  Unknown
};

struct SlotKey {
  unsigned object_id = 0;
  int64_t offset = 0;
  bool unknown = false;

  bool operator<(const SlotKey &Other) const {
    if (object_id != Other.object_id)
      return object_id < Other.object_id;
    if (offset != Other.offset)
      return offset < Other.offset;
    return unknown < Other.unknown;
  }

  bool operator==(const SlotKey &Other) const {
    return object_id == Other.object_id && offset == Other.offset &&
           unknown == Other.unknown;
  }
};

struct AbstractObject {
  unsigned id = 0;
  ObjectKind kind = ObjectKind::Unknown;
  llvm::Value *base = nullptr;
  llvm::Function *owner = nullptr;
  std::string name;
};

class ProgramIndex {
public:
  using SignatureMap = std::map<std::string, std::set<llvm::Function *>>;
  using CallerMap = std::map<llvm::Function *, std::vector<llvm::CallBase *>>;
  using ReturnMap = std::map<llvm::Function *, std::vector<llvm::ReturnInst *>>;

  explicit ProgramIndex(llvm::Module &M);

  const SignatureMap &getAddressTakenBySignature() const {
    return address_taken_by_signature_;
  }
  const std::vector<llvm::CallBase *> &getIndirectCalls() const {
    return indirect_calls_;
  }
  const std::vector<llvm::StoreInst *> &getStores() const { return stores_; }
  const std::vector<llvm::CallBase *> &getMemTransfers() const {
    return mem_transfers_;
  }
  const CallerMap &getDirectCallers() const { return direct_callers_; }
  const ReturnMap &getReturns() const { return returns_; }
  const std::vector<llvm::GlobalVariable *> &getGlobalsWithInitializers() const {
    return globals_with_initializers_;
  }
  const AbstractObject *lookupObject(llvm::Value *V) const;
  const AbstractObject *lookupFormalObject(const llvm::Argument *Arg) const;

  std::string normalizeSignature(llvm::FunctionType *FTy) const;
  std::string getSignature(const llvm::Function *F) const;
  std::string getSignature(const llvm::CallBase *CB) const;

private:
  llvm::Module &module_;
  std::vector<AbstractObject> objects_;
  std::map<llvm::Value *, unsigned> object_lookup_;
  std::map<const llvm::Argument *, unsigned> formal_object_lookup_;
  SignatureMap address_taken_by_signature_;
  std::vector<llvm::CallBase *> indirect_calls_;
  std::vector<llvm::StoreInst *> stores_;
  std::vector<llvm::CallBase *> mem_transfers_;
  CallerMap direct_callers_;
  ReturnMap returns_;
  std::vector<llvm::GlobalVariable *> globals_with_initializers_;

  void collectObjects();
  void collectFunctions();
};

} // namespace dfpa
