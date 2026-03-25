//===-- Verification/Sifa/SifaSymAbs.h ------------------------------------===//
//
// Public API for the SymAbsAI-backed Sifa helper.
//
// This is intentionally narrower than the migrated Sifa engine: it uses
// SymAbsAI as a whole-block transfer engine on a single function's
// CFG. Calls are interpreted by SymAbsAI's own transformers and
// ModuleContext, not by Sifa's interprocedural call-summary machinery.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_SIFASYMABS_H
#define LOTUS_VERIFICATION_SIFA_SIFASYMABS_H

#include "Verification/Sifa/SymAbs/SifaSymAbsOptions.h"

#include <memory>

namespace llvm {
class BasicBlock;
class Function;
class Module;
} // namespace llvm

namespace symabs_ai {
class AbstractValue;
} // namespace symabs_ai

namespace lotus {
namespace sifa {

using SymAbsState = std::shared_ptr<symabs_ai::AbstractValue>;

/// Run the intraprocedural SymAbsAI-backed helper for one function
/// and compute the abstract state at `target` (after phi nodes in `target`).
///
/// Returns a null state for bottom/unreachable.
SymAbsState analyzeSymAbsTo(const llvm::Module &M, const llvm::Function &F,
                            const llvm::BasicBlock &target,
                            const SifaSymAbsOptions &options = {});

/// Convenience wrapper: reachability query using the selected abstract domain.
bool isReachableSymAbs(const llvm::Module &M, const llvm::Function &F,
                       const llvm::BasicBlock &target,
                       const SifaSymAbsOptions &options = {});

/// Compute the abstract state at the procedure exit (the synthetic EXIT node).
SymAbsState analyzeSymAbsToReturn(const llvm::Module &M,
                                  const llvm::Function &F,
                                  const SifaSymAbsOptions &options = {});

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_SIFASYMABS_H
