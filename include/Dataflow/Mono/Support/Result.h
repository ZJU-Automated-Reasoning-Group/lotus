#ifndef LOTUS_DATAFLOW_MONO_SUPPORT_RESULT_H_
#define LOTUS_DATAFLOW_MONO_SUPPORT_RESULT_H_

#include "llvm/IR/Instruction.h"
#include "llvm/IR/Value.h"

#include <map>
#include <set>

namespace mono {

template <typename ContainerT> class DataFlowResultT {
public:
  DataFlowResultT() = default;

  ContainerT &GEN(llvm::Instruction *inst) { return gens[inst]; }
  ContainerT &KILL(llvm::Instruction *inst) { return kills[inst]; }
  ContainerT &IN(llvm::Instruction *inst) { return ins[inst]; }
  ContainerT &OUT(llvm::Instruction *inst) { return outs[inst]; }

  // Const accessors: return a reference to the stored value, or to an empty
  // container if the instruction has no entry.  This allows read-only access
  // on a const DataFlowResultT without inserting default-constructed entries.
  const ContainerT &GEN(llvm::Instruction *inst) const {
    auto it = gens.find(inst);
    return it != gens.end() ? it->second : EmptyContainer;
  }
  const ContainerT &KILL(llvm::Instruction *inst) const {
    auto it = kills.find(inst);
    return it != kills.end() ? it->second : EmptyContainer;
  }
  const ContainerT &IN(llvm::Instruction *inst) const {
    auto it = ins.find(inst);
    return it != ins.end() ? it->second : EmptyContainer;
  }
  const ContainerT &OUT(llvm::Instruction *inst) const {
    auto it = outs.find(inst);
    return it != outs.end() ? it->second : EmptyContainer;
  }

private:
  std::map<llvm::Instruction *, ContainerT> gens;
  std::map<llvm::Instruction *, ContainerT> kills;
  std::map<llvm::Instruction *, ContainerT> ins;
  std::map<llvm::Instruction *, ContainerT> outs;
  ContainerT EmptyContainer{};
};

class DataFlowResult {
public:
  DataFlowResult() = default;

  std::set<llvm::Value *> &GEN(llvm::Instruction *inst) { return gens[inst]; }
  std::set<llvm::Value *> &KILL(llvm::Instruction *inst) { return kills[inst]; }
  std::set<llvm::Value *> &IN(llvm::Instruction *inst) { return ins[inst]; }
  std::set<llvm::Value *> &OUT(llvm::Instruction *inst) { return outs[inst]; }

  // Const accessors — return empty set for missing entries.
  const std::set<llvm::Value *> &GEN(llvm::Instruction *inst) const {
    auto it = gens.find(inst);
    return it != gens.end() ? it->second : EmptySet;
  }
  const std::set<llvm::Value *> &KILL(llvm::Instruction *inst) const {
    auto it = kills.find(inst);
    return it != kills.end() ? it->second : EmptySet;
  }
  const std::set<llvm::Value *> &IN(llvm::Instruction *inst) const {
    auto it = ins.find(inst);
    return it != ins.end() ? it->second : EmptySet;
  }
  const std::set<llvm::Value *> &OUT(llvm::Instruction *inst) const {
    auto it = outs.find(inst);
    return it != outs.end() ? it->second : EmptySet;
  }

private:
  std::map<llvm::Instruction *, std::set<llvm::Value *>> gens;
  std::map<llvm::Instruction *, std::set<llvm::Value *>> kills;
  std::map<llvm::Instruction *, std::set<llvm::Value *>> ins;
  std::map<llvm::Instruction *, std::set<llvm::Value *>> outs;
  std::set<llvm::Value *> EmptySet{};
};

} // namespace mono

#endif // LOTUS_DATAFLOW_MONO_SUPPORT_RESULT_H_
