//===-- Verification/Sifa/CallGraph.h -------------------------------------===//
//
// Call graph for interprocedural Sifa (ported from Ultimate Library-Sifa).
//
// Detects which procedures have to be interpreted to reach a given set of
// locations of interest (LOIs). Aligns with Ultimate's CallGraph:
// - LOIs per procedure, mCalls/mCalledBy, successorsOfInterest,
// - initialProceduresOfInterest, callClosure, relevantProceduresTopsorted.
// Throws if the program is recursive (no topological order).
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_CALLGRAPH_H
#define LOTUS_VERIFICATION_SIFA_CALLGRAPH_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"

#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lotus {
namespace sifa {

/// Call graph for interprocedural Sifa.
///
/// In the LLVM port, interprocedural edges come from direct resolved call
/// targets (`CallBase::getCalledFunction()`). Indirect calls therefore remain
/// within the intraprocedural transfer semantics unless the caller supplies a
/// more precise resolved call graph externally.
class CallGraph {
public:
  using LOI = std::pair<const llvm::Function *, const llvm::BasicBlock *>;

  /// Ultimate-aligned: gather LOIs from all "error" blocks in the module.
  /// Returns (Function, BasicBlock) for each block that contains a call to an
  /// error-like function (e.g. __VERIFIER_error, abort, __assert_fail).
  /// Optional \p errorFunctionNames: additional names to treat as error
  /// (default includes __VERIFIER_error, abort, __assert_fail,
  /// __VERIFIER_abort).
  static std::vector<LOI>
  gatherErrorLocations(const llvm::Module &M,
                       llvm::ArrayRef<llvm::StringRef> errorFunctionNames = {});

  /// Build call graph for \p M with a single initial procedure and
  /// \p locationsOfInterest. Throws if recursive (no topological order).
  CallGraph(const llvm::Module &M, const llvm::Function *entryProcedure,
            const std::vector<LOI> &locationsOfInterest);

  /// Ultimate-aligned: build call graph from all given initial procedures.
  /// Procedures outside \p initialProcedures are ignored unless they are in the
  /// forward call closure of one of those entries.
  CallGraph(const llvm::Module &M,
            llvm::ArrayRef<const llvm::Function *> initialProcedures,
            const std::vector<LOI> &locationsOfInterest);

  /// Procedures that are entry and (contain an LOI or have a successor of
  /// interest).
  std::vector<const llvm::Function *> initialProceduresOfInterest() const;

  /// LOIs inside \p procedure (locations of interest in that procedure).
  std::vector<const llvm::BasicBlock *>
  locationsOfInterest(const llvm::Function &procedure) const;

  /// Callees of \p procedure that lead to an LOI (procedure calls g and g has
  /// LOI or g has successor of interest).
  std::vector<const llvm::Function *>
  successorsOfInterest(const llvm::Function &procedure) const;

  /// Relevant procedures in topological order (caller before callee).
  const std::vector<const llvm::Function *> &
  relevantProceduresTopsorted() const;

private:
  bool hasLoiOrSuccessorWithLoi(const llvm::Function *F) const;
  std::unordered_set<const llvm::Function *>
  callClosure(const std::vector<const llvm::Function *> &procedures) const;

  const llvm::Module *M_ = nullptr;
  std::vector<const llvm::Function *> initialProcedures_;
  /// LOIs inside each procedure (procedure -> LOI blocks).
  std::unordered_map<const llvm::Function *,
                     std::vector<const llvm::BasicBlock *>>
      loisInsideProcedure_;
  /// caller -> callees (f calls g => mCalls[f].count(g))
  std::unordered_map<const llvm::Function *,
                     std::unordered_set<const llvm::Function *>>
      mCalls_;
  /// callee -> callers (f calls g => mCalledBy[g].count(f))
  std::unordered_map<const llvm::Function *,
                     std::unordered_set<const llvm::Function *>>
      mCalledBy_;
  /// (caller, callee) such that caller calls callee and callee has LOI or has
  /// successor of interest.
  std::unordered_map<const llvm::Function *,
                     std::unordered_set<const llvm::Function *>>
      successorsOfInterest_;
  std::vector<const llvm::Function *> topsorted_;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_CALLGRAPH_H
