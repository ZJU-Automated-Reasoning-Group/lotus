#pragma once

#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"

#include "Dataflow/ControlFlow/FlowDirection.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace dataflow {
namespace controlflow {

/// Instruction-level intraprocedural CFG interface (Phasar-like).
class IntraCFG {
public:
  using n_t = llvm::Instruction *;
  using f_t = llvm::Function *;

  virtual ~IntraCFG() = default;

  virtual std::vector<n_t> getSuccsOf(n_t Inst, FlowDirection Dir) const = 0;
  virtual std::vector<n_t> getPredsOf(n_t Inst, FlowDirection Dir) const = 0;

  /// Whether Dst is a branch target (i.e., has >1 predecessors) for the given
  /// direction. Src is provided for parity with Phasar's API.
  virtual bool isBranchTarget(n_t /*Src*/, n_t Dst, FlowDirection Dir) const {
    return getPredsOf(Dst, Dir).size() > 1;
  }

  virtual std::vector<n_t> getAllInstructionsOf(f_t Function) const = 0;
  virtual std::vector<std::pair<n_t, n_t>>
  getAllControlFlowEdges(f_t Function, FlowDirection Dir) const = 0;

  /// Function containing \p Inst. Null if \p Inst is null.
  virtual f_t getFunctionOf(n_t Inst) const = 0;

  /// Start nodes for the given direction. Forward: entry block first
  /// instruction(s). Backward: exit instructions.
  virtual std::vector<n_t> getStartPointsOf(f_t Function,
                                            FlowDirection Dir) const = 0;

  /// Exit nodes for the given direction. Forward: return/exit instructions.
  /// Backward: entry block first instruction(s).
  virtual std::vector<n_t> getExitPointsOf(f_t Function,
                                           FlowDirection Dir) const = 0;

  /// True iff \p Inst is a start node for the given direction.
  virtual bool isStartPoint(n_t Inst, FlowDirection Dir) const = 0;

  /// True iff \p Inst is an exit node for the given direction.
  virtual bool isExitInst(n_t Inst, FlowDirection Dir) const = 0;
};

/// Default LLVM-backed intraprocedural instruction CFG.
class LLVMIntraCFG final : public IntraCFG {
public:
  std::vector<n_t> getSuccsOf(n_t Inst, FlowDirection Dir) const override;
  std::vector<n_t> getPredsOf(n_t Inst, FlowDirection Dir) const override;

  std::vector<n_t> getAllInstructionsOf(f_t Function) const override;
  std::vector<std::pair<n_t, n_t>>
  getAllControlFlowEdges(f_t Function, FlowDirection Dir) const override;

  f_t getFunctionOf(n_t Inst) const override;
  std::vector<n_t> getStartPointsOf(f_t Function,
                                    FlowDirection Dir) const override;
  std::vector<n_t> getExitPointsOf(f_t Function,
                                   FlowDirection Dir) const override;
  bool isStartPoint(n_t Inst, FlowDirection Dir) const override;
  bool isExitInst(n_t Inst, FlowDirection Dir) const override;

private:
  static std::vector<n_t> getForwardSuccs(n_t Inst);
  static std::vector<n_t> getBackwardSuccs(n_t Inst);
  static std::vector<n_t> getForwardStartPoints(f_t Function);
  static std::vector<n_t> getForwardExitPoints(f_t Function);
};

} // namespace controlflow
} // namespace dataflow

// ---- Header-only implementation ----

namespace dataflow {
namespace controlflow {

inline std::vector<LLVMIntraCFG::n_t> LLVMIntraCFG::getForwardSuccs(n_t Inst) {
  std::vector<n_t> Succs;
  if (Inst == nullptr) {
    return Succs;
  }
  if (Inst->isTerminator()) {
    for (auto *SuccBB : llvm::successors(Inst->getParent())) {
      Succs.push_back(&*SuccBB->begin());
    }
    return Succs;
  }
  if (auto *Next = Inst->getNextNode()) {
    Succs.push_back(Next);
  }
  return Succs;
}

inline std::vector<LLVMIntraCFG::n_t> LLVMIntraCFG::getBackwardSuccs(n_t Inst) {
  std::vector<n_t> Preds;
  if (Inst == nullptr) {
    return Preds;
  }
  auto *BB = Inst->getParent();
  if (Inst != &*BB->begin()) {
    Preds.push_back(Inst->getPrevNode());
    return Preds;
  }
  for (auto *PredBB : llvm::predecessors(BB)) {
    Preds.push_back(PredBB->getTerminator());
  }
  return Preds;
}

inline std::vector<LLVMIntraCFG::n_t>
LLVMIntraCFG::getSuccsOf(n_t Inst, FlowDirection Dir) const {
  return Dir == FlowDirection::Forward ? getForwardSuccs(Inst)
                                       : getBackwardSuccs(Inst);
}

inline std::vector<LLVMIntraCFG::n_t>
LLVMIntraCFG::getPredsOf(n_t Inst, FlowDirection Dir) const {
  return Dir == FlowDirection::Forward ? getBackwardSuccs(Inst)
                                       : getForwardSuccs(Inst);
}

inline std::vector<LLVMIntraCFG::n_t>
LLVMIntraCFG::getAllInstructionsOf(f_t Function) const {
  std::vector<n_t> Insts;
  if (Function == nullptr || Function->isDeclaration()) {
    return Insts;
  }
  for (auto &BB : *Function) {
    for (auto &I : BB) {
      Insts.push_back(&I);
    }
  }
  return Insts;
}

inline std::vector<std::pair<LLVMIntraCFG::n_t, LLVMIntraCFG::n_t>>
LLVMIntraCFG::getAllControlFlowEdges(f_t Function, FlowDirection Dir) const {
  std::vector<std::pair<n_t, n_t>> Edges;
  if (Function == nullptr || Function->isDeclaration()) {
    return Edges;
  }
  for (auto &BB : *Function) {
    for (auto &I : BB) {
      for (auto *Succ : getSuccsOf(&I, Dir)) {
        Edges.push_back({&I, Succ});
      }
    }
  }
  return Edges;
}

inline LLVMIntraCFG::f_t LLVMIntraCFG::getFunctionOf(n_t Inst) const {
  return Inst && Inst->getParent() ? Inst->getParent()->getParent() : nullptr;
}

inline std::vector<LLVMIntraCFG::n_t>
LLVMIntraCFG::getForwardStartPoints(f_t Function) {
  std::vector<n_t> Start;
  if (Function == nullptr || Function->isDeclaration() || Function->empty()) {
    return Start;
  }
  Start.push_back(&*Function->getEntryBlock().begin());
  return Start;
}

inline std::vector<LLVMIntraCFG::n_t>
LLVMIntraCFG::getForwardExitPoints(f_t Function) {
  std::vector<n_t> Exit;
  if (Function == nullptr || Function->isDeclaration()) {
    return Exit;
  }
  for (auto &BB : *Function) {
    if (auto *Term = BB.getTerminator()) {
      if (llvm::isa<llvm::ReturnInst>(Term)) {
        Exit.push_back(Term);
      }
    }
  }
  return Exit;
}

inline std::vector<LLVMIntraCFG::n_t>
LLVMIntraCFG::getStartPointsOf(f_t Function, FlowDirection Dir) const {
  if (Dir == FlowDirection::Forward) {
    return getForwardStartPoints(Function);
  }
  return getForwardExitPoints(Function);
}

inline std::vector<LLVMIntraCFG::n_t>
LLVMIntraCFG::getExitPointsOf(f_t Function, FlowDirection Dir) const {
  if (Dir == FlowDirection::Forward) {
    return getForwardExitPoints(Function);
  }
  return getForwardStartPoints(Function);
}

inline bool LLVMIntraCFG::isStartPoint(n_t Inst, FlowDirection Dir) const {
  auto *F = getFunctionOf(Inst);
  if (F == nullptr) {
    return false;
  }
  auto Start = getStartPointsOf(F, Dir);
  return std::find(Start.begin(), Start.end(), Inst) != Start.end();
}

inline bool LLVMIntraCFG::isExitInst(n_t Inst, FlowDirection Dir) const {
  auto *F = getFunctionOf(Inst);
  if (F == nullptr) {
    return false;
  }
  auto Exit = getExitPointsOf(F, Dir);
  return std::find(Exit.begin(), Exit.end(), Inst) != Exit.end();
}

} // namespace controlflow
} // namespace dataflow
