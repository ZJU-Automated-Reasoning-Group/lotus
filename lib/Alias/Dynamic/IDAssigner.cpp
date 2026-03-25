#include "Alias/Dynamic/IDAssigner.h"

#include <llvm/IR/Module.h>
#include <llvm/IR/User.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace dynamic {

static constexpr IDType startID = 1u; ///< Starting ID for value numbering

/// Assigns a unique ID to a value if it doesn't already have one
bool IDAssigner::assignValueID(const Value *v) {
  assert(v != nullptr);

  if (idMap.count(v))
    return false;

  idMap[v] = nextID;
  assert(nextID == startID + revIdMap.size());
  revIdMap.push_back(v);
  ++nextID;
  return true;
}

/// Recursively assigns IDs to a user and all its operands
bool IDAssigner::assignUserID(const User *u) {
  assert(u != nullptr);

  if (!assignValueID(u))
    return false;

  bool changed = false;
  for (auto const &op : u->operands())
    if (auto *child = dyn_cast<User>(&op))
      changed |= assignUserID(child);
  return changed;
}

/// Constructs ID mappings for all values in the module (globals, functions,
/// args, instructions)
IDAssigner::IDAssigner(const Module &module) : nextID(startID) {
  for (auto const &g : module.globals()) {
    assignValueID(&g);
    // if (g.hasInitializer())
    //	assignUserID(g.getInitializer());
  }

  for (auto const &f : module) {
    assignValueID(&f);

    for (auto const &arg : f.args())
      assignValueID(&arg);

    for (auto const &bb : f) {
      // assignValueID(&bb);
      for (auto const &inst : bb)
        assignValueID(&inst);
    }
  }
}

/// Returns the ID for a value, or nullptr if not found
const IDType *IDAssigner::getID(const llvm::Value &v) const {
  auto itr = idMap.find(&v);
  if (itr == idMap.end())
    return nullptr;
  return &itr->second;
}

/// Returns the value for a given ID, or nullptr if invalid
const llvm::Value *IDAssigner::getValue(IDType id) const {
  if (id >= revIdMap.size())
    return nullptr;
  return revIdMap[id - startID];
}

/// Prints all ID-to-value mappings for debugging
void IDAssigner::dump() const {
  for (auto i = 0ul, e = revIdMap.size(); i < e; ++i) {
    errs() << (i + startID) << " => " << revIdMap[i]->getName() << "\n";
  }
}
} // namespace dynamic
