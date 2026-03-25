// Context policy: selective vs full k-CFA.
// Selective = 0-CFA for direct calls, k-CFA for indirect calls.

#include "Alias/TPA/Context/ContextPolicy.h"

#include "Alias/TPA/Context/KLimitContext.h"

#include <llvm/IR/Instructions.h>

using namespace llvm;

namespace context {

static ContextStrategy s_strategy = ContextStrategy::KLimit;

void setContextStrategy(ContextStrategy s) { s_strategy = s; }

ContextStrategy getContextStrategy() { return s_strategy; }

static bool isDirectCall(const Instruction *inst) {
  const auto *cb = dyn_cast<CallBase>(inst);
  return cb && cb->getCalledFunction() != nullptr;
}

const Context *pushContextForCall(const Context *ctx,
                                  const Instruction *callInst) {
  if (s_strategy == ContextStrategy::Selective && isDirectCall(callInst))
    return ctx;
  return KLimitContext::pushContext(ctx, callInst);
}

} // namespace context
