// Implementation of K-Limiting Context Sensitivity.
//
// KLimitContext enforces a maximum depth (k) on the call string.
// This is a standard technique to ensure termination and control the cost of
// analysis.
//
// Logic:
// - When pushing a new context, check the current depth.
// - If depth < k, create a new context extending the current one (via
// Context::pushContext).
// - If depth == k, stop growing. The resulting context is the same as the
// parent,
//   effectively merging the recursive step or deep call chain.

#include "Alias/TPA/Context/KLimitContext.h"

#include "Alias/TPA/Context/ProgramPoint.h"

using namespace llvm;

namespace context {

const Context *KLimitContext::pushContext(const ProgramPoint &pp) {
  return pushContext(pp.getContext(), pp.getInstruction());
}

const Context *KLimitContext::pushContext(const Context *ctx,
                                          const Instruction *inst) {
  size_t k = defaultLimit;

  assert(ctx->size() <= k);

  // If we reached the limit, do not create a new context node.
  // Return the current context, effectively flattening the
  // recursion/call-chain.
  if (ctx->size() == k)
    return ctx;
  else
    // Otherwise, delegate to the base Context implementation to extend the
    // chain.
    return Context::pushContext(ctx, inst);
}

} // namespace context
