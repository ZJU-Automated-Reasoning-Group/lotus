#pragma once

#include <functional>
#include <map>
#include <set>
#include <vector>

#include <llvm/ADT/DenseMap.h>
#include <llvm/IR/Instructions.h>
#include <z3++.h>

using namespace llvm;

// Forward declaration
namespace kint {
struct crange;
} // namespace kint

namespace kint {

enum class interr {
  NONE, // Uninitialized/invalid state
  INT_OVERFLOW,
  DIV_BY_ZERO,
  BAD_SHIFT,
  ARRAY_OOB,
  DEAD_TRUE_BR,
  DEAD_FALSE_BR
};

// Structure to hold information about a program point along an execution path
struct PathPoint {
  const BasicBlock *bb;
  const Instruction *inst;
  std::string description;

  PathPoint(const BasicBlock *bb, const Instruction *inst = nullptr,
            const std::string &desc = "")
      : bb(bb), inst(inst), description(desc) {}
};

// Structure to hold the execution path that leads to a bug
struct BugPath {
  std::vector<PathPoint> path;
  const Instruction *bugInstruction;
  interr bugType;

  BugPath() : bugInstruction(nullptr), bugType(interr::NONE) {}
  BugPath(const Instruction *bugInst, interr type)
      : bugInstruction(bugInst), bugType(type) {}
};

using BugKey = std::pair<const Instruction *, interr>;

class BugDetection {
public:
  BugDetection() = default;
  ~BugDetection() = default;

  // Error marking
  template <interr err_t, typename I>
  static typename std::enable_if<std::is_pointer<I>::value>::type
  mark_err(I inst);

  template <interr err_t, typename I>
  static typename std::enable_if<!std::is_pointer<I>::value>::type
  mark_err(I &inst);

  // SMT-based bug detection
  void
  binary_check(BinaryOperator *op, z3::solver &solver,
               const DenseMap<const Value *, llvm::Optional<z3::expr>> &v2sym,
               std::set<Instruction *> &overflow_insts,
               std::set<Instruction *> &bad_shift_insts,
               std::set<Instruction *> &div_zero_insts,
               bool robust_mode = false,
               const std::vector<z3::expr> *path_constraints = nullptr,
               const std::vector<z3::expr> *universal_vars = nullptr,
               const std::function<void(interr, const z3::expr &)> &dump = {},
               const std::function<bool(interr)> &robustFilter = {});

  // SMT expression generation
  z3::expr binary_op_propagate(
      BinaryOperator *op,
      const DenseMap<const Value *, llvm::Optional<z3::expr>> &v2sym,
      z3::solver &solver);
  z3::expr cast_op_propagate(
      CastInst *op,
      const DenseMap<const Value *, llvm::Optional<z3::expr>> &v2sym,
      z3::solver &solver);
  z3::expr
  v2sym(const Value *v,
        const DenseMap<const Value *, llvm::Optional<z3::expr>> &v2sym_map,
        z3::solver &solver);

  // Range constraint generation
  bool add_range_cons(
      const crange &rng, const z3::expr &bv, z3::solver &solver,
      const std::function<void(const z3::expr &)> &addConstraint = {});

  // Path tracking
  void setCurrentPath(const std::vector<PathPoint> &path) {
    m_current_path = path;
  }
  void addPathPoint(const PathPoint &point) { m_current_path.push_back(point); }
  void clearCurrentPath() { m_current_path.clear(); }
  const std::vector<PathPoint> &getCurrentPath() const {
    return m_current_path;
  }
  void clearState() {
    m_current_path.clear();
    m_bug_paths.clear();
  }

  // Get bug paths
  const std::map<BugKey, BugPath> &getBugPaths() const {
    return m_bug_paths;
  }

  // Record a bug using the currently-tracked path.
  void recordBug(const Instruction *inst, interr type);

  // Error reporting
  void mark_errors(const std::map<ICmpInst *, bool> &impossible_branches,
                   const std::set<GetElementPtrInst *> &gep_oob,
                   const std::set<Instruction *> &overflow_insts,
                   const std::set<Instruction *> &bad_shift_insts,
                   const std::set<Instruction *> &div_zero_insts);

  // SARIF reporting
  void
  generateSarifReport(const std::string &filename,
                      const std::map<ICmpInst *, bool> &impossible_branches,
                      const std::set<GetElementPtrInst *> &gep_oob,
                      const std::set<Instruction *> &overflow_insts,
                      const std::set<Instruction *> &bad_shift_insts,
                      const std::set<Instruction *> &div_zero_insts);

private:
  // Store the current execution path being analyzed
  std::vector<PathPoint> m_current_path;

  // Map from (bug instruction, bug type) to its execution path.
  std::map<BugKey, BugPath> m_bug_paths;

  // Helper to record a bug with its path
  void recordBugWithPath(const Instruction *inst, interr type);
};

} // namespace kint
