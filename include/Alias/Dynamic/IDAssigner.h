#pragma once

#include <cstdint>
#include <vector>

#include <llvm/ADT/DenseMap.h>

namespace llvm {
class Module;
class Value;
class User;
} // namespace llvm

namespace dynamic {

using IDType = std::uint32_t;

class IDAssigner {
private:
  IDType nextID;

  using MapType = llvm::DenseMap<const llvm::Value *, IDType>;
  MapType idMap;
  using RevMapType = std::vector<const llvm::Value *>;
  RevMapType revIdMap;

  bool assignValueID(const llvm::Value *);
  bool assignUserID(const llvm::User *);

public:
  IDAssigner(const llvm::Module &);

  const IDType *getID(const llvm::Value &v) const;
  const llvm::Value *getValue(IDType id) const;

  void dump() const;
};
} // namespace dynamic
