#pragma once

#include <llvm/ADT/DenseSet.h>
#include <llvm/IR/DataLayout.h>

namespace llvm {
class Module;
class Type;
} // namespace llvm

namespace tpa {

class TypeSet {
private:
  using SetType = llvm::DenseSet<llvm::Type *>;
  SetType typeSet;

  llvm::DataLayout dataLayout;

public:
  using const_iterator = SetType::const_iterator;

  TypeSet(const llvm::Module &module) : dataLayout(&module) {}

  bool insert(llvm::Type *ty) { return typeSet.insert(ty).second; }

  bool count(llvm::Type *ty) const { return typeSet.count(ty); }

  const llvm::DataLayout &getDataLayout() const { return dataLayout; }

  const_iterator begin() const { return typeSet.begin(); }
  const_iterator end() const { return typeSet.end(); }
};

} // namespace tpa
