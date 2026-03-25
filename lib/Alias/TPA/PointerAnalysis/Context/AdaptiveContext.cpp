// Implementation of Adaptive Context Sensitivity.
//
// AdaptiveContext allows fine-grained control over which call sites induce a
// context change. Instead of applying context sensitivity uniformly (like
// K-Limit), it only creates new contexts for "tracked" call sites.
//
// Use Cases:
// - Focusing analysis on specific modules or high-risk functions.
// - Reducing overhead by treating helper functions context-insensitively.
//
// Logic:
// - `trackedCallsites`: A set of program points (call sites) that should
// trigger context expansion.
// - `pushContext`: Checks if the call site is in the tracked set.
//   - If yes: Pushes a new context (context-sensitive).
//   - If no: Returns the current context (context-insensitive propagation).

#include "Alias/TPA/Context/AdaptiveContext.h"

using namespace llvm;

namespace context {

// Marks a specific call site as "interesting" for context sensitivity.
void AdaptiveContext::trackCallSite(const ProgramPoint &pLoc) {
  trackedCallsites.insert(pLoc);
}

const Context *AdaptiveContext::pushContext(const Context *ctx,
                                            const llvm::Instruction *inst) {
  return pushContext(ProgramPoint(ctx, inst));
}

// Decides whether to create a new context based on the call site.
const Context *AdaptiveContext::pushContext(const ProgramPoint &pp) {
  // If this call site is tracked, proceed with standard context creation.
  if (trackedCallsites.count(pp))
    return Context::pushContext(pp);
  else
    // Otherwise, reuse the existing context (context-insensitive).
    return pp.getContext();
}

} // namespace context
