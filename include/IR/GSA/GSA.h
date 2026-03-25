//===- GSA.h - Gated SSA construction interfaces --------------*- C++ -*-===//
//
// This file declares the public interfaces for constructing a read-only Gated
// Single-Assignment (GSA) view over LLVM IR. The analysis augments SSA by
// introducing immutable gating expressions that encode the control flow
// guarding values flowing into join points.
//
// The implementation lives in lib/IR/GSA and is intentionally independent
// from verification-specific utilities so it can be reused by general IR
// analyses and transformations.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>
#include <vector>

namespace llvm {
class BasicBlock;
class ConstantInt;
class LoopInfo;
class PHINode;
class Type;
class Value;
} // namespace llvm

namespace gsa {

enum class GateKind { Gamma, Mu, Eta };

enum class GuardKind {
  Unconditional,
  BranchTrue,
  BranchFalse,
  SwitchCase,
  SwitchDefault,
  InvokeNormal,
  InvokeUnwind,
  Opaque
};

class GateGuard {
public:
  GateGuard() = default;

  GateGuard(GuardKind kind, llvm::BasicBlock *control_block,
            llvm::BasicBlock *successor, llvm::Value *condition = nullptr,
            llvm::ConstantInt *case_value = nullptr)
      : m_kind(kind), m_control_block(control_block), m_successor(successor),
        m_condition(condition), m_case_value(case_value) {}

  GuardKind getKind() const { return m_kind; }
  llvm::BasicBlock *getControlBlock() const { return m_control_block; }
  llvm::BasicBlock *getSuccessor() const { return m_successor; }
  llvm::Value *getCondition() const { return m_condition; }
  llvm::ConstantInt *getCaseValue() const { return m_case_value; }

private:
  GuardKind m_kind{GuardKind::Opaque};
  llvm::BasicBlock *m_control_block{nullptr};
  llvm::BasicBlock *m_successor{nullptr};
  llvm::Value *m_condition{nullptr};
  llvm::ConstantInt *m_case_value{nullptr};
};

class GateExpr {
public:
  enum class Kind { Bottom, LeafValue, Select, Switch };

  struct SwitchArm {
    GateGuard guard;
    const GateExpr *expr{nullptr};
  };

  GateExpr(llvm::Type *type, llvm::Value *leaf_value);
  explicit GateExpr(llvm::Type *type);
  GateExpr(llvm::Type *type, GateGuard true_guard, GateGuard false_guard,
           const GateExpr *true_expr, const GateExpr *false_expr);
  GateExpr(llvm::Type *type, std::vector<SwitchArm> switch_arms);

  Kind getKind() const { return m_kind; }
  llvm::Type *getType() const { return m_type; }
  llvm::Value *getLeafValue() const { return m_leaf_value; }
  const GateGuard &getTrueGuard() const { return m_true_guard; }
  const GateGuard &getFalseGuard() const { return m_false_guard; }
  const GateExpr *getTrueExpr() const { return m_true_expr; }
  const GateExpr *getFalseExpr() const { return m_false_expr; }
  llvm::ArrayRef<SwitchArm> getSwitchArms() const { return m_switch_arms; }

private:
  Kind m_kind{Kind::Bottom};
  llvm::Type *m_type{nullptr};
  llvm::Value *m_leaf_value{nullptr};
  GateGuard m_true_guard;
  GateGuard m_false_guard;
  const GateExpr *m_true_expr{nullptr};
  const GateExpr *m_false_expr{nullptr};
  std::vector<SwitchArm> m_switch_arms;
};

class GateNode {
public:
  GateNode(llvm::PHINode *source_phi, GateKind kind, llvm::Type *type,
           const GateExpr *root_expr, bool is_lowerable)
      : m_source_phi(source_phi), m_kind(kind), m_type(type),
        m_root_expr(root_expr), m_is_lowerable(is_lowerable) {}

  llvm::PHINode *getSourcePhi() const { return m_source_phi; }
  GateKind getKind() const { return m_kind; }
  llvm::Type *getType() const { return m_type; }
  const GateExpr *getRootExpr() const { return m_root_expr; }
  bool isLowerable() const { return m_is_lowerable; }

private:
  llvm::PHINode *m_source_phi{nullptr};
  GateKind m_kind{GateKind::Gamma};
  llvm::Type *m_type{nullptr};
  const GateExpr *m_root_expr{nullptr};
  bool m_is_lowerable{false};
};

/// Exposes block-level control dependence information.
class ControlDependenceAnalysis {
public:
  virtual ~ControlDependenceAnalysis() = default;

  /// Returns true when the block is tracked by the analysis. Unreachable
  /// blocks are not tracked.
  virtual bool isTracked(const llvm::BasicBlock &BB) const = 0;

  /// All blocks that \p BB is control dependent on, sorted in reverse
  /// topological order. Returns an empty array for untracked blocks.
  virtual llvm::ArrayRef<llvm::BasicBlock *>
  getCDBlocks(llvm::BasicBlock *BB) const = 0;

  /// Returns true if there is a CFG path from \p Src to \p Dst.
  virtual bool isReachable(llvm::BasicBlock *Src,
                           llvm::BasicBlock *Dst) const = 0;

  /// Returns an integer that respects the topological ordering of the CFG.
  /// Callers must check isTracked() before querying it.
  virtual unsigned getBBTopoIdx(llvm::BasicBlock *BB) const = 0;
};

/// Module pass wrapper for ControlDependenceAnalysis.
class ControlDependenceAnalysisPass : public llvm::ModulePass {
public:
  static char ID;

  ControlDependenceAnalysisPass();

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
  bool runOnFunction(llvm::Function &F);
  bool runOnModule(llvm::Module &M) override;

  llvm::StringRef getPassName() const override;
  void print(llvm::raw_ostream &os, const llvm::Module *M) const override;

  bool hasAnalysisFor(const llvm::Function &F) const;
  ControlDependenceAnalysis &
  getControlDependenceAnalysis(const llvm::Function &F);

private:
  llvm::DenseMap<const llvm::Function *,
                 std::unique_ptr<ControlDependenceAnalysis>>
      m_analyses;
};

llvm::ModulePass *createControlDependenceAnalysisPass();

/// Exposes the immutable GSA mapping for a function.
class GateAnalysis {
public:
  virtual ~GateAnalysis() = default;

  /// Returns true if \p PN has a tracked GSA gate.
  virtual bool hasGate(const llvm::PHINode &PN) const = 0;

  /// Returns the gate associated with \p PN.
  virtual const GateNode &getGate(const llvm::PHINode &PN) const = 0;

  /// Returns all gates in deterministic function order. Returned pointers stay
  /// valid for the lifetime of this GateAnalysis object.
  virtual llvm::ArrayRef<const GateNode *> gates() const = 0;

  /// True if thinned gating was requested.
  virtual bool isThinned() const = 0;
};

/// Module pass that builds immutable GSA data for all functions.
class GateAnalysisPass : public llvm::ModulePass {
public:
  static char ID;

  GateAnalysisPass();
  explicit GateAnalysisPass(bool thinned);

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
  bool runOnFunction(llvm::Function &F, ControlDependenceAnalysis &CDA,
                     llvm::LoopInfo &LI);
  bool runOnModule(llvm::Module &M) override;

  llvm::StringRef getPassName() const override;
  void print(llvm::raw_ostream &os, const llvm::Module *M) const override;

  bool hasAnalysisFor(const llvm::Function &F) const;
  GateAnalysis &getGateAnalysis(const llvm::Function &F);

private:
  bool m_thinned{true};
  llvm::DenseMap<const llvm::Function *, std::unique_ptr<GateAnalysis>>
      m_analyses;
};

llvm::ModulePass *createGateAnalysisPass();

/// Module pass that materializes GSA expressions as LLVM IR.
class GsaMaterializationPass : public llvm::ModulePass {
public:
  static char ID;

  GsaMaterializationPass();
  explicit GsaMaterializationPass(bool replace_phis);

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
  bool runOnFunction(llvm::Function &F, GateAnalysis &GA);
  bool runOnModule(llvm::Module &M) override;

  llvm::StringRef getPassName() const override;
  void print(llvm::raw_ostream &os, const llvm::Module *M) const override;

private:
  bool m_replace_phis{true};
};

llvm::ModulePass *createGsaMaterializationPass();

} // namespace gsa
