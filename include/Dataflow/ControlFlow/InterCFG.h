#pragma once

#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include "Dataflow/ControlFlow/IntraCFG.h"

#include <functional>
#include <map>
#include <vector>

namespace dataflow {
namespace controlflow {

/// Instruction-level interprocedural CFG interface (Phasar-like).
class InterCFG : public IntraCFG {
public:
  using m_t = llvm::Module *;
  using IntraCFG::getExitPointsOf;
  using IntraCFG::getStartPointsOf;
  using IntraCFG::isExitInst;

  virtual ~InterCFG() = default;

  virtual bool isCallSite(n_t Inst) const = 0;
  virtual bool isExitInst(n_t Inst) const = 0;

  virtual std::vector<n_t> getStartPointsOf(f_t Callee) const = 0;
  virtual std::vector<n_t> getExitPointsOf(f_t Callee) const = 0;

  virtual std::vector<n_t> getReturnSitesOfCallAt(n_t CallSite) const = 0;

  virtual std::vector<f_t> getCalleesOfCallAt(n_t CallSite) const = 0;
  virtual std::vector<n_t> getCallersOf(f_t Callee) const = 0;

  virtual m_t getModule() const = 0;
};

/// Default LLVM-backed interprocedural CFG with pluggable callee resolution.
///
/// getCallersOf() now uses a pre-built caller index (O(1) per
/// query) instead of scanning every instruction in the module on every call
/// (O(N) per query → O(N²) overall for context-insensitive analyses).
class LLVMInterCFG final : public InterCFG {
public:
  using GetCalleesFn = std::function<std::vector<f_t>(n_t)>;

  explicit LLVMInterCFG(m_t M, GetCalleesFn GetCallees = {});

  bool isCallSite(n_t Inst) const override;
  bool isExitInst(n_t Inst) const override;

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

  std::vector<n_t> getStartPointsOf(f_t Callee) const override;
  std::vector<n_t> getExitPointsOf(f_t Callee) const override;

  std::vector<n_t> getReturnSitesOfCallAt(n_t CallSite) const override;

  std::vector<f_t> getCalleesOfCallAt(n_t CallSite) const override;
  std::vector<n_t> getCallersOf(f_t Callee) const override;

  m_t getModule() const override { return Mod; }

private:
  static std::vector<n_t> continuationInstructions(n_t CallInst);

  /// Build the caller index: Callee → list of call-site instructions.
  /// Called once in the constructor so getCallersOf() is O(1).
  void buildCallerIndex();

  m_t Mod = nullptr;
  GetCalleesFn GetCallees;
  LLVMIntraCFG Intra;
  /// Pre-built caller index for O(1) getCallersOf() queries.
  std::map<f_t, std::vector<n_t>> CallerIndex;
};

} // namespace controlflow
} // namespace dataflow

// ---- Header-only implementation ----

namespace dataflow {
namespace controlflow {

inline LLVMInterCFG::LLVMInterCFG(m_t M, GetCalleesFn GetCallees)
    : Mod(M), GetCallees(std::move(GetCallees)) {
  if (!this->GetCallees) {
    this->GetCallees = [](n_t Inst) -> std::vector<f_t> {
      std::vector<f_t> Callees;
      auto *Call = llvm::dyn_cast_or_null<llvm::CallBase>(Inst);
      if (Call == nullptr) {
        return Callees;
      }
      if (auto *Callee = Call->getCalledFunction()) {
        Callees.push_back(Callee);
      }
      return Callees;
    };
  }
  // build the caller index once at construction time.
  buildCallerIndex();
}

inline void LLVMInterCFG::buildCallerIndex() {
  CallerIndex.clear();
  if (Mod == nullptr) {
    return;
  }
  for (auto &F : *Mod) {
    if (F.isDeclaration()) {
      continue;
    }
    for (auto &BB : F) {
      for (auto &I : BB) {
        if (!isCallSite(&I)) {
          continue;
        }
        for (auto *Callee : getCalleesOfCallAt(&I)) {
          if (Callee != nullptr) {
            CallerIndex[Callee].push_back(&I);
          }
        }
      }
    }
  }
}

inline bool LLVMInterCFG::isCallSite(n_t Inst) const {
  return llvm::isa<llvm::CallBase>(Inst);
}

inline bool LLVMInterCFG::isExitInst(n_t Inst) const {
  return llvm::isa<llvm::ReturnInst>(Inst);
}

inline std::vector<LLVMInterCFG::n_t>
LLVMInterCFG::getSuccsOf(n_t Inst, FlowDirection Dir) const {
  return Intra.getSuccsOf(Inst, Dir);
}

inline std::vector<LLVMInterCFG::n_t>
LLVMInterCFG::getPredsOf(n_t Inst, FlowDirection Dir) const {
  return Intra.getPredsOf(Inst, Dir);
}

inline std::vector<LLVMInterCFG::n_t>
LLVMInterCFG::getAllInstructionsOf(f_t Function) const {
  return Intra.getAllInstructionsOf(Function);
}

inline std::vector<std::pair<LLVMInterCFG::n_t, LLVMInterCFG::n_t>>
LLVMInterCFG::getAllControlFlowEdges(f_t Function, FlowDirection Dir) const {
  return Intra.getAllControlFlowEdges(Function, Dir);
}

inline LLVMInterCFG::f_t LLVMInterCFG::getFunctionOf(n_t Inst) const {
  return Intra.getFunctionOf(Inst);
}

inline std::vector<LLVMInterCFG::n_t>
LLVMInterCFG::getStartPointsOf(f_t Function, FlowDirection Dir) const {
  return Intra.getStartPointsOf(Function, Dir);
}

inline std::vector<LLVMInterCFG::n_t>
LLVMInterCFG::getExitPointsOf(f_t Function, FlowDirection Dir) const {
  return Intra.getExitPointsOf(Function, Dir);
}

inline bool LLVMInterCFG::isStartPoint(n_t Inst, FlowDirection Dir) const {
  return Intra.isStartPoint(Inst, Dir);
}

inline bool LLVMInterCFG::isExitInst(n_t Inst, FlowDirection Dir) const {
  return Intra.isExitInst(Inst, Dir);
}

inline std::vector<LLVMInterCFG::n_t>
LLVMInterCFG::getStartPointsOf(f_t Callee) const {
  std::vector<n_t> Starts;
  if (Callee == nullptr || Callee->isDeclaration() || Callee->empty()) {
    return Starts;
  }
  Starts.push_back(&*Callee->getEntryBlock().begin());
  return Starts;
}

inline std::vector<LLVMInterCFG::n_t>
LLVMInterCFG::getExitPointsOf(f_t Callee) const {
  std::vector<n_t> Exits;
  if (Callee == nullptr || Callee->isDeclaration()) {
    return Exits;
  }
  for (auto &BB : *Callee) {
    if (auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(BB.getTerminator())) {
      Exits.push_back(Ret);
    }
  }
  return Exits;
}

inline std::vector<LLVMInterCFG::n_t>
LLVMInterCFG::continuationInstructions(n_t CallInst) {
  std::vector<n_t> Continuations;
  auto *Call = llvm::dyn_cast_or_null<llvm::CallBase>(CallInst);
  if (Call == nullptr) {
    return Continuations;
  }
  if (auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(CallInst)) {
    Continuations.push_back(&*Invoke->getNormalDest()->begin());
    return Continuations;
  }
  if (auto *CallBr = llvm::dyn_cast<llvm::CallBrInst>(CallInst)) {
    for (unsigned I = 0, E = CallBr->getNumSuccessors(); I < E; ++I) {
      Continuations.push_back(&*CallBr->getSuccessor(I)->begin());
    }
    return Continuations;
  }
  if (auto *Next = CallInst->getNextNode()) {
    Continuations.push_back(Next);
  }
  return Continuations;
}

inline std::vector<LLVMInterCFG::n_t>
LLVMInterCFG::getReturnSitesOfCallAt(n_t CallSite) const {
  return continuationInstructions(CallSite);
}

inline std::vector<LLVMInterCFG::f_t>
LLVMInterCFG::getCalleesOfCallAt(n_t CallSite) const {
  return GetCallees ? GetCallees(CallSite) : std::vector<f_t>{};
}

inline std::vector<LLVMInterCFG::n_t>
LLVMInterCFG::getCallersOf(f_t Callee) const {
  // O(1) lookup via pre-built index instead of O(N) scan.
  if (Callee == nullptr) {
    return {};
  }
  auto It = CallerIndex.find(Callee);
  if (It == CallerIndex.end()) {
    return {};
  }
  return It->second;
}

} // namespace controlflow
} // namespace dataflow
